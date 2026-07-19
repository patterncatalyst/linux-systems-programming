// fwatch v3 — the ch09 watcher, now sandboxed.
//
// `watch --sandbox DIR` applies two independent kernel access-control layers
// before the epoll loop ever runs:
//
//   Landlock  — a ruleset that restricts filesystem READS to DIR and nothing
//               else. It is an unprivileged, self-imposed sandbox: the kernel
//               enforces it against this process and every descendant, and it
//               cannot be lifted short of exec-ing something that never asked
//               for it in the first place.
//   seccomp   — a syscall allowlist (libseccomp) covering only the syscalls
//               the watch loop actually issues (epoll/inotify/timerfd/
//               signalfd plumbing, read/write/close, the allocator). Anything
//               else returns EPERM instead of running.
//
// `probe --sandbox DIR --outside PATH` and `probe --forbidden-syscall` are
// negative controls that apply ONE of the two layers in isolation and prove
// it actually denies what it claims to: a read outside the Landlock tree, and
// a syscall outside the seccomp allowlist.
//
// Landlock ABI has no libc wrapper; every call here goes through the raw
// syscall(2) numbers in <linux/landlock.h> / <sys/syscall.h>. The ABI is
// probed at runtime (LANDLOCK_CREATE_RULESET_VERSION) rather than assumed, in
// line with "don't hardcode the ABI" — a program built against a newer
// kernel's headers must still degrade sanely on an older one.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/landlock.h>
#include <seccomp.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;
using std::chrono::steady_clock;

constexpr auto kDebounce = milliseconds{100};

[[nodiscard]] std::error_code last_error() {
    return {errno, std::system_category()};
}

// ---------------------------------------------------------------------------
// RAII file descriptor — the only owner of every fd in this program.
// ---------------------------------------------------------------------------

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_{fd} {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
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

private:
    int fd_ = -1;
};

[[nodiscard]] std::expected<Fd, std::error_code> checked(int fd) {
    if (fd < 0) {
        return std::unexpected(last_error());
    }
    return Fd{fd};
}

// ---------------------------------------------------------------------------
// Landlock — raw syscalls, no libc wrapper exists.
// ---------------------------------------------------------------------------

[[nodiscard]] long sys_landlock_create_ruleset(const landlock_ruleset_attr* attr, std::size_t size,
                                                std::uint32_t flags) {
    return ::syscall(SYS_landlock_create_ruleset, attr, size, flags);
}

[[nodiscard]] long sys_landlock_add_rule(int ruleset_fd, landlock_rule_type type,
                                          const void* attr, std::uint32_t flags) {
    return ::syscall(SYS_landlock_add_rule, ruleset_fd, type, attr, flags);
}

[[nodiscard]] long sys_landlock_restrict_self(int ruleset_fd, std::uint32_t flags) {
    return ::syscall(SYS_landlock_restrict_self, ruleset_fd, flags);
}

// Probe the running kernel's supported Landlock ABI. Returns 0 if Landlock
// is not built in / not enabled at boot (never hardcoded).
[[nodiscard]] int landlock_abi() {
    const long v =
        sys_landlock_create_ruleset(nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    return v < 0 ? 0 : static_cast<int>(v);
}

// Create a ruleset that handles READ_FILE + READ_DIR (available since ABI
// v1), add one path-beneath rule granting exactly those rights under `dir`,
// then restrict_self. On success the calling process — and everything it
// execs from here on — can only read inside `dir`.
[[nodiscard]] std::expected<void, std::error_code> apply_landlock(const std::string& dir) {
    constexpr std::uint64_t access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    landlock_ruleset_attr attr{.handled_access_fs = access, .handled_access_net = 0, .scoped = 0};

    const long rs = sys_landlock_create_ruleset(&attr, sizeof(attr), 0);
    if (rs < 0) {
        return std::unexpected(last_error());
    }
    Fd ruleset{static_cast<int>(rs)};

    auto dirfd = checked(::open(dir.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!dirfd) {
        return std::unexpected(dirfd.error());
    }
    landlock_path_beneath_attr pb{.allowed_access = access, .parent_fd = dirfd->get()};
    if (sys_landlock_add_rule(ruleset.get(), LANDLOCK_RULE_PATH_BENEATH, &pb, 0) < 0) {
        return std::unexpected(last_error());
    }
    dirfd->reset();

    // Landlock requires no_new_privs (or CAP_SYS_ADMIN) before restrict_self.
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        return std::unexpected(last_error());
    }
    if (sys_landlock_restrict_self(ruleset.get(), 0) < 0) {
        return std::unexpected(last_error());
    }
    return {};
}

// ---------------------------------------------------------------------------
// seccomp — libseccomp allowlist covering exactly what the watch loop needs.
// ---------------------------------------------------------------------------

// Syscalls the epoll/inotify/timerfd/signalfd watch loop issues, empirically
// confirmed with `strace -f` across a full watch session (create/modify/
// delete + debounce flush + timeout exit). Anything not on this list is
// denied with EPERM once the filter loads.
constexpr std::array kWatchSyscalls{
    SCMP_SYS(read),           SCMP_SYS(write),          SCMP_SYS(close),
    SCMP_SYS(inotify_init1),  SCMP_SYS(inotify_add_watch), SCMP_SYS(timerfd_create),
    SCMP_SYS(timerfd_settime), SCMP_SYS(signalfd4),     SCMP_SYS(epoll_create1),
    SCMP_SYS(epoll_ctl),      SCMP_SYS(epoll_wait),     SCMP_SYS(rt_sigprocmask),
    SCMP_SYS(mmap),           SCMP_SYS(munmap),         SCMP_SYS(mremap),
    SCMP_SYS(brk),            SCMP_SYS(rt_sigreturn),   SCMP_SYS(exit_group),
    SCMP_SYS(exit),           SCMP_SYS(fstat),          SCMP_SYS(newfstatat),
    SCMP_SYS(lseek),          SCMP_SYS(clock_gettime),  SCMP_SYS(clock_nanosleep),
    SCMP_SYS(nanosleep),      SCMP_SYS(getrandom),
};

// Installs an allow-listed set of syscalls (`allowed`); everything else
// returns errno `deny_errno`. Returns the number of syscalls admitted.
[[nodiscard]] std::expected<std::size_t, std::error_code>
install_seccomp(const int* allowed, std::size_t count, int deny_errno) {
    scmp_filter_ctx ctx = ::seccomp_init(SCMP_ACT_ERRNO(deny_errno));
    if (ctx == nullptr) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (::seccomp_rule_add(ctx, SCMP_ACT_ALLOW, allowed[i], 0) < 0) {
            ::seccomp_release(ctx);
            return std::unexpected(last_error());
        }
    }
    const int loaded = ::seccomp_load(ctx);
    ::seccomp_release(ctx);
    if (loaded < 0) {
        return std::unexpected(std::error_code{-loaded, std::system_category()});
    }
    return count;
}

// ---------------------------------------------------------------------------
// v0: snapshot + diff
// ---------------------------------------------------------------------------

struct FileState {
    std::string name;
    std::uint64_t size;
    std::int64_t mtime_ns;
};

[[nodiscard]] std::expected<std::vector<FileState>, std::error_code>
scan_dir(const std::string& dir) {
    std::error_code ec;
    std::vector<FileState> out;
    for (const auto& entry : fs::directory_iterator{dir, ec}) {
        struct stat st{};
        if (::fstatat(AT_FDCWD, entry.path().c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            continue; // raced with a concurrent delete
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        out.push_back(FileState{
            .name = entry.path().filename().string(),
            .size = static_cast<std::uint64_t>(st.st_size),
            .mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 +
                        st.st_mtim.tv_nsec,
        });
    }
    if (ec) {
        return std::unexpected(ec);
    }
    std::ranges::sort(out, {}, &FileState::name);
    return out;
}

int cmd_snapshot(const std::string& dir) {
    const auto files = scan_dir(dir);
    if (!files) {
        std::println(stderr, "fwatch: snapshot: {}: {}", dir, files.error().message());
        return 1;
    }
    for (const auto& f : *files) {
        std::println("{}\t{}\t{}", f.name, f.size, f.mtime_ns);
    }
    return 0;
}

[[nodiscard]] std::expected<std::map<std::string, std::string>, std::error_code>
load_snapshot(const std::string& path) {
    std::ifstream in{path};
    if (!in) {
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }
    std::map<std::string, std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        const auto tab2 = line.rfind('\t');
        if (tab2 == std::string::npos || tab2 == 0) {
            continue;
        }
        const auto tab1 = line.rfind('\t', tab2 - 1);
        if (tab1 == std::string::npos) {
            continue;
        }
        out.insert_or_assign(line.substr(0, tab1), line.substr(tab1 + 1));
    }
    return out;
}

int cmd_diff(const std::string& old_path, const std::string& new_path) {
    const auto old_snap = load_snapshot(old_path);
    if (!old_snap) {
        std::println(stderr, "fwatch: diff: {}: {}", old_path, old_snap.error().message());
        return 1;
    }
    const auto new_snap = load_snapshot(new_path);
    if (!new_snap) {
        std::println(stderr, "fwatch: diff: {}: {}", new_path, new_snap.error().message());
        return 1;
    }
    std::set<std::string> names;
    for (const auto& [name, _] : *old_snap) {
        names.insert(name);
    }
    for (const auto& [name, _] : *new_snap) {
        names.insert(name);
    }
    for (const auto& name : names) {
        const auto o = old_snap->find(name);
        const auto n = new_snap->find(name);
        if (o == old_snap->end()) {
            std::println("created {}", name);
        } else if (n == new_snap->end()) {
            std::println("deleted {}", name);
        } else if (o->second != n->second) {
            std::println("modified {}", name);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// v1: watch — one epoll loop, three fds, zero threads beyond main.
// ---------------------------------------------------------------------------

enum class Kind : std::uint8_t { created, modified, deleted };

[[nodiscard]] std::string_view kind_name(Kind k) {
    switch (k) {
    case Kind::created: return "created";
    case Kind::modified: return "modified";
    case Kind::deleted: return "deleted";
    }
    return "?";
}

[[nodiscard]] std::optional<Kind> classify(std::uint32_t mask) {
    if ((mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
        return Kind::created;
    }
    if ((mask & (IN_DELETE | IN_MOVED_FROM)) != 0) {
        return Kind::deleted;
    }
    if ((mask & (IN_MODIFY | IN_ATTRIB)) != 0) {
        return Kind::modified;
    }
    return std::nullopt;
}

[[nodiscard]] Kind merge(std::optional<Kind> old_kind, Kind new_kind) {
    if (!old_kind) {
        return new_kind;
    }
    if (new_kind == Kind::deleted) {
        return Kind::deleted;
    }
    if (*old_kind == Kind::created) {
        return Kind::created;
    }
    return Kind::modified;
}

struct Pending {
    Kind kind;
    steady_clock::time_point due;
};

[[nodiscard]] std::expected<Fd, std::error_code> make_signalfd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
        return std::unexpected(last_error());
    }
    return checked(::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
}

[[nodiscard]] std::expected<void, std::error_code> arm_timer(const Fd& tfd, nanoseconds rel) {
    if (rel <= nanoseconds::zero()) {
        rel = nanoseconds{1};
    }
    itimerspec spec{};
    spec.it_value.tv_sec = rel.count() / 1'000'000'000;
    spec.it_value.tv_nsec = rel.count() % 1'000'000'000;
    if (::timerfd_settime(tfd.get(), 0, &spec, nullptr) != 0) {
        return std::unexpected(last_error());
    }
    return {};
}

void flush(std::map<std::string, Pending>& pending, steady_clock::time_point now, bool all) {
    for (auto it = pending.begin(); it != pending.end();) {
        if (all || it->second.due <= now) {
            std::println("event: {} {}", kind_name(it->second.kind), it->first);
            it = pending.erase(it);
        } else {
            ++it;
        }
    }
    std::fflush(stdout);
}

// The watch loop itself. Everything it needs (Landlock + seccomp, if
// requested) has already been set up by the caller before this runs; the
// loop's own logic never needs to know whether it is sandboxed.
int run_watch_loop(const std::string& dir, int timeout_ms) {
    auto inotify = checked(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
    if (!inotify) {
        std::println(stderr, "fwatch: watch: inotify_init1: {}", inotify.error().message());
        return 1;
    }
    constexpr std::uint32_t watch_mask =
        IN_CREATE | IN_MODIFY | IN_ATTRIB | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
    if (::inotify_add_watch(inotify->get(), dir.c_str(), watch_mask) < 0) {
        std::println(stderr, "fwatch: watch: {}: {}", dir, last_error().message());
        return 1;
    }
    auto timer = checked(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    if (!timer) {
        std::println(stderr, "fwatch: watch: timerfd_create: {}", timer.error().message());
        return 1;
    }
    auto sigfd = make_signalfd();
    if (!sigfd) {
        std::println(stderr, "fwatch: watch: signalfd: {}", sigfd.error().message());
        return 1;
    }
    auto epoll = checked(::epoll_create1(EPOLL_CLOEXEC));
    if (!epoll) {
        std::println(stderr, "fwatch: watch: epoll_create1: {}", epoll.error().message());
        return 1;
    }

    enum : std::uint32_t { tok_inotify, tok_timer, tok_signal };
    const auto add = [&](const Fd& fd,
                         std::uint32_t token) -> std::expected<void, std::error_code> {
        epoll_event ev{.events = EPOLLIN, .data = {.u32 = token}};
        if (::epoll_ctl(epoll->get(), EPOLL_CTL_ADD, fd.get(), &ev) != 0) {
            return std::unexpected(last_error());
        }
        return {};
    };
    auto registered = add(*inotify, tok_inotify)
                          .and_then([&] { return add(*timer, tok_timer); })
                          .and_then([&] { return add(*sigfd, tok_signal); });
    if (!registered) {
        std::println(stderr, "fwatch: watch: epoll_ctl: {}", registered.error().message());
        return 1;
    }

    std::println(stderr, "fwatch: watching {}", dir);
    const auto deadline = steady_clock::now() + milliseconds{timeout_ms};
    std::map<std::string, Pending> pending;

    for (;;) {
        auto next = deadline;
        for (const auto& [_, p] : pending) {
            next = std::min(next, p.due);
        }
        if (auto armed = arm_timer(*timer, next - steady_clock::now()); !armed) {
            std::println(stderr, "fwatch: watch: timerfd_settime: {}", armed.error().message());
            return 1;
        }

        std::array<epoll_event, 8> events{};
        const int n = ::epoll_wait(epoll->get(), events.data(), events.size(), -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::println(stderr, "fwatch: watch: epoll_wait: {}", last_error().message());
            return 1;
        }

        for (int i = 0; i < n; ++i) {
            switch (events[i].data.u32) {
            case tok_inotify: {
                alignas(inotify_event) char buf[4096];
                for (;;) {
                    const ssize_t got = ::read(inotify->get(), buf, sizeof buf);
                    if (got <= 0) {
                        break;
                    }
                    const auto now = steady_clock::now();
                    for (const char* p = buf; p < buf + got;) {
                        const auto* ev = reinterpret_cast<const inotify_event*>(p);
                        p += sizeof(inotify_event) + ev->len;
                        if (ev->len == 0) {
                            continue;
                        }
                        const auto kind = classify(ev->mask);
                        if (!kind) {
                            continue;
                        }
                        const std::string name{ev->name};
                        const auto it = pending.find(name);
                        const auto old_kind = it == pending.end()
                                                  ? std::nullopt
                                                  : std::optional{it->second.kind};
                        pending.insert_or_assign(
                            name, Pending{merge(old_kind, *kind), now + kDebounce});
                    }
                }
                break;
            }
            case tok_timer: {
                std::uint64_t expirations = 0;
                std::ignore = ::read(timer->get(), &expirations, sizeof expirations);
                const auto now = steady_clock::now();
                if (now >= deadline) {
                    flush(pending, now, /*all=*/true);
                    std::println("fwatch: exiting (timeout)");
                    return 0;
                }
                flush(pending, now, /*all=*/false);
                break;
            }
            case tok_signal: {
                signalfd_siginfo info{};
                std::ignore = ::read(sigfd->get(), &info, sizeof info);
                flush(pending, steady_clock::now(), /*all=*/true);
                std::println("fwatch: exiting (signal)");
                return 0;
            }
            default:
                break;
            }
        }
    }
}

int cmd_watch(const std::string& dir, int timeout_ms, bool sandbox) {
    if (sandbox) {
        const int abi = landlock_abi();
        if (abi <= 0) {
            std::println(stderr, "fwatch: watch: Landlock not supported by this kernel");
            return 1;
        }
        if (auto ll = apply_landlock(dir); !ll) {
            std::println(stderr, "fwatch: watch: landlock: {}", ll.error().message());
            return 1;
        }
        std::println(stderr, "fwatch: landlock ABI={} enforced", abi);

        const auto installed =
            install_seccomp(kWatchSyscalls.data(), kWatchSyscalls.size(), EPERM);
        if (!installed) {
            std::println(stderr, "fwatch: watch: seccomp: {}", installed.error().message());
            return 1;
        }
        std::println(stderr, "fwatch: seccomp filter installed ({} syscalls allowed)",
                     *installed);
    }
    return run_watch_loop(dir, timeout_ms);
}

// ---------------------------------------------------------------------------
// probes — negative controls, one Landlock, one seccomp, exercised alone.
// ---------------------------------------------------------------------------

// Exit codes for the probes are distinct from the ordinary 0/1/2 contract so
// a verifier can tell "confirmed denied" (the PASSING case for a negative
// control) apart from "ran into some other error" or "was NOT denied" (a
// sandbox bug).
constexpr int kProbeDenied = 20;    // Landlock/seccomp did deny it, as expected
constexpr int kProbeNotDenied = 21; // sandbox failed to deny it — a real bug

int cmd_probe_outside(const std::string& sandbox_dir, const std::string& outside_path) {
    const int abi = landlock_abi();
    if (abi <= 0) {
        std::println(stderr, "fwatch: probe: Landlock not supported by this kernel");
        return 1;
    }
    if (auto ll = apply_landlock(sandbox_dir); !ll) {
        std::println(stderr, "fwatch: probe: landlock: {}", ll.error().message());
        return 1;
    }
    std::println(stderr, "fwatch: landlock ABI={} enforced", abi);

    errno = 0;
    const int fd = ::open(outside_path.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::close(fd);
        std::println("fwatch: probe outside {}: opened (landlock did NOT block this)",
                     outside_path);
        return kProbeNotDenied;
    }
    const auto ec = last_error();
    if (ec == std::errc::permission_denied) {
        std::println("fwatch: probe outside {}: EACCES ({})", outside_path, ec.message());
        return kProbeDenied;
    }
    std::println("fwatch: probe outside {}: unexpected error: {}", outside_path, ec.message());
    return 1;
}

int cmd_probe_forbidden_syscall() {
    // Deliberately omit socket(2) from the allowlist.
    const auto installed =
        install_seccomp(kWatchSyscalls.data(), kWatchSyscalls.size(), EPERM);
    if (!installed) {
        std::println(stderr, "fwatch: probe: seccomp: {}", installed.error().message());
        return 1;
    }
    std::println(stderr, "fwatch: seccomp filter installed ({} syscalls allowed)", *installed);

    errno = 0;
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        ::close(s);
        std::println("fwatch: probe forbidden-syscall: socket() unexpectedly succeeded");
        return kProbeNotDenied;
    }
    const auto ec = last_error();
    if (ec == std::errc::operation_not_permitted) {
        std::println("fwatch: probe forbidden-syscall: EPERM ({})", ec.message());
        return kProbeDenied;
    }
    std::println("fwatch: probe forbidden-syscall: unexpected error: {}", ec.message());
    return 1;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

int usage() {
    std::println(stderr, "usage: fwatch <command>");
    std::println(stderr,
                 "  snapshot DIR                              one line per regular file");
    std::println(stderr,
                 "  diff OLD NEW                              compare two snapshots");
    std::println(stderr,
                 "  watch DIR [--timeout-ms T]                unsandboxed watch");
    std::println(stderr,
                 "  watch --sandbox DIR [--timeout-ms T]      Landlock+seccomp sandboxed watch");
    std::println(stderr,
                 "  probe --sandbox DIR --outside PATH        negative control: open PATH "
                 "(outside DIR) under Landlock");
    std::println(stderr,
                 "  probe --forbidden-syscall                 negative control: socket(2) "
                 "under a seccomp allowlist that omits it");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args{argv + 1, argv + argc};
    if (args.empty()) {
        return usage();
    }
    const auto cmd = args[0];

    if (cmd == "snapshot" && args.size() == 2) {
        return cmd_snapshot(std::string{args[1]});
    }
    if (cmd == "diff" && args.size() == 3) {
        return cmd_diff(std::string{args[1]}, std::string{args[2]});
    }
    if (cmd == "watch") {
        bool sandbox = false;
        std::optional<std::string> dir;
        int timeout_ms = 2000;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--sandbox" && i + 1 < args.size()) {
                sandbox = true;
                dir = std::string{args[++i]};
            } else if (args[i] == "--timeout-ms" && i + 1 < args.size()) {
                try {
                    timeout_ms = std::stoi(std::string{args[++i]});
                } catch (const std::exception&) {
                    return usage();
                }
                if (timeout_ms <= 0) {
                    return usage();
                }
            } else if (!dir) {
                dir = std::string{args[i]};
            } else {
                return usage();
            }
        }
        if (!dir) {
            return usage();
        }
        return cmd_watch(*dir, timeout_ms, sandbox);
    }
    if (cmd == "probe") {
        std::optional<std::string> sandbox_dir;
        std::optional<std::string> outside;
        bool forbidden_syscall = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--sandbox" && i + 1 < args.size()) {
                sandbox_dir = std::string{args[++i]};
            } else if (args[i] == "--outside" && i + 1 < args.size()) {
                outside = std::string{args[++i]};
            } else if (args[i] == "--forbidden-syscall") {
                forbidden_syscall = true;
            } else {
                return usage();
            }
        }
        if (forbidden_syscall && !sandbox_dir && !outside) {
            return cmd_probe_forbidden_syscall();
        }
        if (sandbox_dir && outside && !forbidden_syscall) {
            return cmd_probe_outside(*sandbox_dir, *outside);
        }
        return usage();
    }
    return usage();
}
