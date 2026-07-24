// fwatch.cpp — the book's recurring file watcher (ch07 polling -> ch09
// inotify/epoll -> ch33 Landlock+seccomp), reduced here to what the fleet
// needs: watch a directory, print one line per raw create/write/delete
// inotify event (no debounce/merge — go/fwatch.go prints every event as it
// arrives, and this port matches that exactly), and optionally run under a
// Landlock ruleset restricting reads to that directory. The raw
// landlock_create_ruleset/add_rule/restrict_self syscalls and the RAII Fd
// pattern are carried over verbatim from ch33's main.cpp (<linux/landlock.h>
// has no libc wrapper); full seccomp allow-listing was proven once there and
// is not re-derived here — capability-bounding-set drop (caps.cpp) plus
// Landlock are this capstone's sandboxing layers, applied by pmon before it
// execs fwatch.
#include "fwatch.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <print>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/landlock.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "util.hpp"

namespace fwatch {

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string errno_msg() { return std::string(std::strerror(errno)); }

// RAII file descriptor — same shape as every other example's Fd/Socket type.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }

    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = -1;
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

private:
    int fd_ = -1;
};

// ---------------------------------------------------------------------------
// Landlock — raw syscalls, no libc wrapper exists (ch33's technique).
// ---------------------------------------------------------------------------

[[nodiscard]] long sys_landlock_create_ruleset(const landlock_ruleset_attr* attr, std::size_t size,
                                                std::uint32_t flags) {
    return ::syscall(SYS_landlock_create_ruleset, attr, size, flags);
}

[[nodiscard]] long sys_landlock_add_rule(int ruleset_fd, landlock_rule_type type, const void* attr,
                                          std::uint32_t flags) {
    return ::syscall(SYS_landlock_add_rule, ruleset_fd, type, attr, flags);
}

[[nodiscard]] long sys_landlock_restrict_self(int ruleset_fd, std::uint32_t flags) {
    return ::syscall(SYS_landlock_restrict_self, ruleset_fd, flags);
}

// Probes the running kernel's supported Landlock ABI; 0 if Landlock is not
// built in / not enabled at boot (never hardcoded).
[[nodiscard]] int landlock_abi() {
    const long v = sys_landlock_create_ruleset(nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    return v < 0 ? 0 : static_cast<int>(v);
}

// Creates a ruleset handling READ_FILE + READ_DIR, adds one path-beneath rule
// granting exactly those rights under `dir`, then restrict_self. On success
// this process — and everything it execs from here on — can only read
// inside `dir`.
[[nodiscard]] bool apply_landlock(const std::string& dir, std::string& err) {
    constexpr std::uint64_t access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    landlock_ruleset_attr attr{.handled_access_fs = access, .handled_access_net = 0, .scoped = 0};

    const long rs = sys_landlock_create_ruleset(&attr, sizeof(attr), 0);
    if (rs < 0) {
        err = "landlock_create_ruleset: " + errno_msg();
        return false;
    }
    Fd ruleset(static_cast<int>(rs));

    const int dirfd = ::open(dir.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) {
        err = "open " + dir + ": " + errno_msg();
        return false;
    }
    landlock_path_beneath_attr pb{.allowed_access = access, .parent_fd = dirfd};
    const long added = sys_landlock_add_rule(ruleset.get(), LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    ::close(dirfd);
    if (added < 0) {
        err = "landlock_add_rule: " + errno_msg();
        return false;
    }

    // Landlock requires no_new_privs (or CAP_SYS_ADMIN) before restrict_self.
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        err = "prctl(PR_SET_NO_NEW_PRIVS): " + errno_msg();
        return false;
    }
    if (sys_landlock_restrict_self(ruleset.get(), 0) < 0) {
        err = "landlock_restrict_self: " + errno_msg();
        return false;
    }
    return true;
}

// classify mirrors go/fwatch.go's eventName: create, then delete, then
// modify (CLOSE_WRITE or MODIFY), else "other" — checked in that priority
// order because a single inotify event's mask can carry more than one bit.
[[nodiscard]] std::string_view classify(std::uint32_t mask) {
    if (mask & IN_CREATE) return "create";
    if (mask & IN_DELETE) return "delete";
    if (mask & IN_CLOSE_WRITE) return "modify";
    if (mask & IN_MODIFY) return "modify";
    return "other";
}

} // namespace

int snapshot(const std::string& dir) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) {
        std::println(stderr, "fwatch: readdir {}: {}", dir, ec.message());
        return 1;
    }
    // os.ReadDir (the Go reference) returns entries sorted by filename;
    // fs::directory_iterator makes no such guarantee, so the names are
    // collected and sorted explicitly to keep this deterministic across
    // languages.
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file()) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::ranges::sort(names);
    for (const auto& name : names) {
        std::println("{}", name);
    }
    return 0;
}

int watch(const std::string& dir, bool sandbox, int timeout_ms) {
    if (sandbox) {
        const int abi = landlock_abi();
        if (abi == 0) {
            std::println(stderr, "fwatch: Landlock not supported by this kernel");
            return 1;
        }
        std::string err;
        if (!apply_landlock(dir, err)) {
            std::println(stderr, "fwatch: landlock: {}", err);
            return 1;
        }
        std::println(stderr, "fwatch: landlock ABI={} enforced dir={}", abi, dir);
    }

    Fd ifd(::inotify_init1(IN_CLOEXEC | IN_NONBLOCK));
    if (!ifd.valid()) {
        std::println(stderr, "fwatch: inotify_init1: {}", errno_msg());
        return 1;
    }
    constexpr std::uint32_t mask = IN_CREATE | IN_MODIFY | IN_DELETE | IN_CLOSE_WRITE;
    if (::inotify_add_watch(ifd.get(), dir.c_str(), mask) < 0) {
        std::println(stderr, "fwatch: inotify_add_watch {}: {}", dir, errno_msg());
        return 1;
    }

    std::println(stderr, "fwatch: watching {} (sandbox={})", dir, sandbox ? "true" : "false");
    std::fflush(stderr);

    auto& sig = util::install_signal_flag();
    const std::optional<std::chrono::steady_clock::time_point> deadline =
        timeout_ms > 0 ? std::optional(std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms))
                       : std::nullopt;

    alignas(struct inotify_event) char buf[4096];
    for (;;) {
        if (sig.load(std::memory_order_relaxed)) {
            std::println("(signal)");
            return 0;
        }
        if (deadline && std::chrono::steady_clock::now() > *deadline) {
            std::println("(timeout)");
            return 0;
        }

        pollfd pfd{.fd = ifd.get(), .events = POLLIN, .revents = 0};
        const int pr = ::poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::println(stderr, "fwatch: poll: {}", errno_msg());
            return 1;
        }
        if (pr == 0) {
            continue;
        }

        const ssize_t nr = ::read(ifd.get(), buf, sizeof(buf));
        if (nr <= 0) {
            continue;
        }
        std::size_t off = 0;
        while (off + sizeof(struct inotify_event) <= static_cast<std::size_t>(nr)) {
            const auto* ev = reinterpret_cast<const struct inotify_event*>(buf + off);
            off += sizeof(struct inotify_event) + ev->len;
            if (ev->len == 0) {
                continue;
            }
            std::println("event: {} {}", classify(ev->mask), (fs::path(dir) / ev->name).string());
            std::fflush(stdout);
        }
    }
}

} // namespace fwatch
