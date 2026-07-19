// fwatch v1 — a file watcher growing chapter by chapter.
//
// v0 subcommands (snapshot/diff) still work; v1 adds `watch`, built as ONE
// epoll loop over three kernel file descriptors on a single thread:
//
//   inotify fd   — filesystem events for the watched directory
//   timerfd      — the debounce (100 ms per path) AND the overall timeout
//   signalfd     — SIGINT/SIGTERM delivered as ordinary readable data
//
// The readiness model is explicit: nothing here blocks except epoll_wait(2).

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
#include <signal.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
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

// Snapshot line -> (name, "size\tmtime_ns"); malformed lines are skipped.
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

// Coalescing rule shared by all three implementations: within one debounce
// window, delete wins, a fresh creation stays "created" through later writes,
// and a delete+recreate pair reads as "modified".
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
        rel = nanoseconds{1}; // 0 would disarm; fire "immediately" instead
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
    // std::map iterates in name order, so a batch flush is deterministic.
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

int cmd_watch(const std::string& dir, int timeout_ms) {
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
        // One timer covers both jobs: it is always armed to whichever comes
        // first — the overall timeout or the earliest per-path debounce.
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
                        break; // EAGAIN: queue drained
                    }
                    const auto now = steady_clock::now();
                    for (const char* p = buf; p < buf + got;) {
                        const auto* ev = reinterpret_cast<const inotify_event*>(p);
                        p += sizeof(inotify_event) + ev->len;
                        if (ev->len == 0) {
                            continue; // event on the directory itself
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

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

int usage() {
    std::println(stderr, "usage: fwatch <command>");
    std::println(stderr,
                 "  snapshot DIR                  one line per regular file: "
                 "name<TAB>size<TAB>mtime_ns");
    std::println(stderr,
                 "  diff OLD NEW                  compare two snapshots: "
                 "created|modified|deleted <name>");
    std::println(stderr,
                 "  watch DIR [--timeout-ms T]    watch DIR (default 2000 ms) until timeout "
                 "or SIGINT/SIGTERM");
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
    if (cmd == "watch" && (args.size() == 2 || args.size() == 4)) {
        int timeout_ms = 2000;
        if (args.size() == 4) {
            if (args[2] != "--timeout-ms") {
                return usage();
            }
            try {
                timeout_ms = std::stoi(std::string{args[3]});
            } catch (const std::exception&) {
                return usage();
            }
            if (timeout_ms <= 0) {
                return usage();
            }
        }
        return cmd_watch(std::string{args[1]}, timeout_ms);
    }
    return usage();
}
