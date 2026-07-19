// pmon v2 — a tiny process supervisor grown chapter by chapter.
//
// v0 (`run`) spawns a command, waits, and mirrors its exit status.
// v1 (`supervise --engine sigchld`) restarts a crashing child, driven by
//     SIGCHLD delivered as readable data through a signalfd.
// v2 (`supervise --engine pidfd`, the DEFAULT) drops the SIGCHLD dependence
//     entirely: pidfd_open(2) turns the child into a pollable file
//     descriptor (raw SYS_pidfd_open on purpose — the glibc wrapper is
//     recent), poll(2) reports the exit as ordinary readability,
//     waitid(P_PIDFD) reaps exactly that child, and the stop path signals
//     through the pidfd with pidfd_send_signal(2) — no pid-reuse race
//     anywhere on the path.

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

[[nodiscard]] std::error_code last_error() {
    return {errno, std::system_category()};
}

// ---------------------------------------------------------------------------
// RAII owners — every fd and every spawn attribute has exactly one.
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

class SpawnAttrs {
public:
    SpawnAttrs() { ::posix_spawnattr_init(&attr_); }
    SpawnAttrs(const SpawnAttrs&) = delete;
    SpawnAttrs& operator=(const SpawnAttrs&) = delete;
    ~SpawnAttrs() { ::posix_spawnattr_destroy(&attr_); }
    [[nodiscard]] posix_spawnattr_t* get() noexcept { return &attr_; }

private:
    posix_spawnattr_t attr_{};
};

// ---------------------------------------------------------------------------
// Spawning and status plumbing shared by every subcommand.
// ---------------------------------------------------------------------------

// The supervisor blocks signals for its signalfd; the child must NOT inherit
// that mask (a supervised `sleep` with SIGTERM blocked would never stop), so
// posix_spawn resets the child's mask to empty.
[[nodiscard]] std::expected<pid_t, std::error_code>
spawn_child(const std::vector<std::string>& cmd) {
    std::vector<char*> argv;
    argv.reserve(cmd.size() + 1);
    for (const auto& s : cmd) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);

    SpawnAttrs attrs;
    sigset_t empty;
    sigemptyset(&empty);
    ::posix_spawnattr_setsigmask(attrs.get(), &empty);
    ::posix_spawnattr_setflags(attrs.get(), POSIX_SPAWN_SETSIGMASK);

    pid_t pid{};
    const int rc = ::posix_spawnp(&pid, argv[0], nullptr, attrs.get(), argv.data(), environ);
    if (rc != 0) {
        return std::unexpected(std::error_code{rc, std::system_category()});
    }
    return pid;
}

struct ChildStatus {
    bool exited;  // true: value is the exit status; false: value is the signal
    int value;
};

[[nodiscard]] ChildStatus status_from(const siginfo_t& si) {
    if (si.si_code == CLD_EXITED) {
        return {.exited = true, .value = si.si_status};
    }
    return {.exited = false, .value = si.si_status};  // CLD_KILLED / CLD_DUMPED
}

// Prints the exit-observation line and returns the mirrored exit code.
int report_exit(pid_t pid, ChildStatus st) {
    if (st.exited) {
        std::println("pmon: child={} exited status={}", pid, st.value);
    } else {
        std::println("pmon: child={} killed signal={}", pid, st.value);
    }
    std::fflush(stdout);
    return st.exited ? st.value : 128 + st.value;
}

// Block `sigs` process-wide and return a signalfd that delivers them as data.
[[nodiscard]] std::expected<Fd, std::error_code>
make_signalfd(std::initializer_list<int> sigs) {
    sigset_t mask;
    sigemptyset(&mask);
    for (const int s : sigs) {
        sigaddset(&mask, s);
    }
    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
        return std::unexpected(last_error());
    }
    return checked(::signalfd(-1, &mask, SFD_CLOEXEC));
}

// ---------------------------------------------------------------------------
// v0: run — spawn, wait, mirror.
// ---------------------------------------------------------------------------

int cmd_run(const std::vector<std::string>& cmd) {
    const auto pid = spawn_child(cmd);
    if (!pid) {
        std::println(stderr, "pmon: spawn: {}: {}", cmd[0], pid.error().message());
        return 1;
    }
    std::println(stderr, "pmon: run child={}", *pid);
    siginfo_t si{};
    while (::waitid(P_PID, static_cast<id_t>(*pid), &si, WEXITED) != 0) {
        if (errno != EINTR) {
            std::println(stderr, "pmon: waitid: {}", last_error().message());
            return 1;
        }
    }
    return report_exit(*pid, status_from(si));
}

// ---------------------------------------------------------------------------
// v1/v2: supervise — two engines, one restart policy.
// ---------------------------------------------------------------------------

enum class Engine : std::uint8_t { pidfd, sigchld };
enum class Stop : std::uint8_t { signal, timeout };
using Outcome = std::variant<ChildStatus, Stop>;

[[nodiscard]] int remaining_ms(steady_clock::time_point deadline) {
    const auto left =
        std::chrono::duration_cast<milliseconds>(deadline - steady_clock::now()).count();
    return left < 0 ? 0 : static_cast<int>(left);
}

// waitid(P_PIDFD) reaps the process the fd refers to — never a recycled pid.
[[nodiscard]] std::expected<ChildStatus, std::error_code> reap_pidfd(const Fd& pidfd) {
    siginfo_t si{};
    while (::waitid(P_PIDFD, static_cast<id_t>(pidfd.get()), &si, WEXITED) != 0) {
        if (errno != EINTR) {
            return std::unexpected(last_error());
        }
    }
    return status_from(si);
}

// pidfd engine: the child's exit is just readability on a file descriptor.
[[nodiscard]] std::expected<Outcome, std::error_code>
wait_pidfd(const Fd& pidfd, const Fd& sigfd, steady_clock::time_point deadline) {
    for (;;) {
        std::array<pollfd, 2> fds{{
            {.fd = pidfd.get(), .events = POLLIN, .revents = 0},
            {.fd = sigfd.get(), .events = POLLIN, .revents = 0},
        }};
        const int n = ::poll(fds.data(), fds.size(), remaining_ms(deadline));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(last_error());
        }
        if (n == 0) {
            return Stop::timeout;
        }
        if ((fds[1].revents & POLLIN) != 0) {
            signalfd_siginfo info{};
            std::ignore = ::read(sigfd.get(), &info, sizeof info);
            return Stop::signal;
        }
        if (fds[0].revents != 0) {
            return reap_pidfd(pidfd).transform([](ChildStatus st) { return Outcome{st}; });
        }
    }
}

// sigchld engine: SIGCHLD (and INT/TERM) arrive through the signalfd.
[[nodiscard]] std::expected<Outcome, std::error_code>
wait_sigchld(pid_t pid, const Fd& sigfd, steady_clock::time_point deadline) {
    for (;;) {
        pollfd pfd{.fd = sigfd.get(), .events = POLLIN, .revents = 0};
        const int n = ::poll(&pfd, 1, remaining_ms(deadline));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(last_error());
        }
        if (n == 0) {
            return Stop::timeout;
        }
        signalfd_siginfo info{};
        std::ignore = ::read(sigfd.get(), &info, sizeof info);
        if (info.ssi_signo != SIGCHLD) {
            return Stop::signal;
        }
        siginfo_t si{};
        si.si_pid = 0;
        if (::waitid(P_PID, static_cast<id_t>(pid), &si, WEXITED | WNOHANG) != 0) {
            return std::unexpected(last_error());
        }
        if (si.si_pid == pid) {
            return Outcome{status_from(si)};
        }
        // Coalesced or stale SIGCHLD: our child is still running.
    }
}

// Ask the child to terminate, reap it, and report why we are leaving.
int finish_stopped(Engine engine, pid_t pid, const Fd& pidfd, Stop why) {
    std::expected<ChildStatus, std::error_code> st;
    if (engine == Engine::pidfd) {
        std::ignore = ::syscall(SYS_pidfd_send_signal, pidfd.get(), SIGTERM, nullptr, 0);
        st = reap_pidfd(pidfd);
    } else {
        std::ignore = ::kill(pid, SIGTERM);
        siginfo_t si{};
        for (;;) {
            if (::waitid(P_PID, static_cast<id_t>(pid), &si, WEXITED) == 0) {
                st = status_from(si);
                break;
            }
            if (errno != EINTR) {
                st = std::unexpected(last_error());
                break;
            }
        }
    }
    if (!st) {
        std::println(stderr, "pmon: waitid: {}", st.error().message());
        return 1;
    }
    std::ignore = report_exit(pid, *st);
    std::println("pmon: exiting ({})", why == Stop::timeout ? "timeout" : "signal");
    std::fflush(stdout);
    return 0;
}

int cmd_supervise(Engine engine, int max_restarts, int timeout_ms,
                  const std::vector<std::string>& cmd) {
    auto sigfd = engine == Engine::pidfd ? make_signalfd({SIGINT, SIGTERM})
                                         : make_signalfd({SIGCHLD, SIGINT, SIGTERM});
    if (!sigfd) {
        std::println(stderr, "pmon: signalfd: {}", sigfd.error().message());
        return 1;
    }
    const auto deadline = steady_clock::now() + milliseconds{timeout_ms};
    int restarts = 0;
    for (;;) {
        const auto pid = spawn_child(cmd);
        if (!pid) {
            std::println(stderr, "pmon: spawn: {}: {}", cmd[0], pid.error().message());
            return 1;
        }

        Fd pidfd;
        std::expected<Outcome, std::error_code> outcome;
        if (engine == Engine::pidfd) {
            auto pf = checked(static_cast<int>(::syscall(SYS_pidfd_open, *pid, 0)));
            if (!pf) {
                std::println(stderr, "pmon: pidfd_open: {}", pf.error().message());
                return 1;
            }
            pidfd = std::move(*pf);
            std::println(stderr, "pmon: engine=pidfd child={} pidfd={}", *pid, pidfd.get());
            outcome = wait_pidfd(pidfd, *sigfd, deadline);
        } else {
            std::println(stderr, "pmon: engine=sigchld child={}", *pid);
            outcome = wait_sigchld(*pid, *sigfd, deadline);
        }
        if (!outcome) {
            std::println(stderr, "pmon: wait: {}", outcome.error().message());
            return 1;
        }
        if (const auto* stop = std::get_if<Stop>(&*outcome)) {
            return finish_stopped(engine, *pid, pidfd, *stop);
        }

        const auto st = std::get<ChildStatus>(*outcome);
        std::ignore = report_exit(*pid, st);
        if (st.exited && st.value == 0) {
            return 0;
        }
        if (restarts >= max_restarts) {
            std::println("pmon: giving up after {} restarts", max_restarts);
            std::fflush(stdout);
            return 1;
        }
        ++restarts;
        std::println("pmon: restart {}/{}", restarts, max_restarts);
        std::fflush(stdout);
    }
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

int usage() {
    std::println(stderr, "usage: pmon <command>");
    std::println(stderr,
                 "  run -- CMD [ARGS...]                     spawn CMD, wait, mirror its exit");
    std::println(stderr,
                 "  supervise [--engine pidfd|sigchld] [--max-restarts N] [--timeout-ms T]");
    std::println(stderr,
                 "            -- CMD [ARGS...]               restart CMD on abnormal exit");
    std::println(stderr,
                 "                                           (defaults: pidfd, N=3, T=10000)");
    return 2;
}

[[nodiscard]] std::optional<int> parse_int(std::string_view s) {
    int value{};
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args{argv + 1, argv + argc};
    if (args.empty()) {
        return usage();
    }
    const auto sep = std::ranges::find(args, std::string_view{"--"});
    if (sep == args.end() || sep + 1 == args.end()) {
        return usage();  // both subcommands need `-- CMD [ARGS...]`
    }
    const std::vector<std::string_view> flags{args.begin() + 1, sep};
    std::vector<std::string> cmd;
    for (auto it = sep + 1; it != args.end(); ++it) {
        cmd.emplace_back(*it);
    }

    if (args[0] == "run") {
        if (!flags.empty()) {
            return usage();
        }
        return cmd_run(cmd);
    }
    if (args[0] == "supervise") {
        auto engine = Engine::pidfd;
        int max_restarts = 3;
        int timeout_ms = 10'000;
        for (std::size_t i = 0; i < flags.size(); i += 2) {
            if (i + 1 >= flags.size()) {
                return usage();
            }
            const auto flag = flags[i];
            const auto value = flags[i + 1];
            if (flag == "--engine") {
                if (value == "pidfd") {
                    engine = Engine::pidfd;
                } else if (value == "sigchld") {
                    engine = Engine::sigchld;
                } else {
                    return usage();
                }
            } else if (flag == "--max-restarts") {
                const auto n = parse_int(value);
                if (!n || *n < 0) {
                    return usage();
                }
                max_restarts = *n;
            } else if (flag == "--timeout-ms") {
                const auto t = parse_int(value);
                if (!t || *t <= 0) {
                    return usage();
                }
                timeout_ms = *t;
            } else {
                return usage();
            }
        }
        return cmd_supervise(engine, max_restarts, timeout_ms, cmd);
    }
    return usage();
}
