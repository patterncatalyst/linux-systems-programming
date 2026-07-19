// pmon v1 — a mini process supervisor (chapter 12: signals and safe handling).
//
//   pmon run -- CMD [ARGS...]
//   pmon supervise [--max-restarts N] [--backoff-ms B] -- CMD [ARGS...]
//
// The C++ take on async-signal-safe design: install NO signal handlers at
// all. SIGTERM/SIGINT/SIGHUP/SIGCHLD are blocked with sigprocmask(2) and
// converted into ordinary file-descriptor reads via signalfd(2), consumed
// from a plain poll(2) loop. No code ever runs in signal context, so nothing
// has to be async-signal-safe — except the tiny window in the forked child
// between fork() and execvp(), which restores an empty signal mask (a
// blocked, inherited SIGTERM would survive exec and make the supervised
// child unkillable).

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono;

class UniqueFd {
  public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~UniqueFd() { reset(); }
    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }

  private:
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    int fd_ = -1;
};

[[nodiscard]] std::error_code errno_ec() {
    return {errno, std::generic_category()};
}

[[noreturn]] void die(std::string_view what, std::error_code ec) {
    std::println(stderr, "pmon: error: {}: {}", what, ec.message());
    std::exit(1);
}

[[noreturn]] void usage() {
    std::println(stderr, "usage: pmon run -- CMD [ARGS...]");
    std::println(stderr,
                 "       pmon supervise [--max-restarts N] [--backoff-ms B] "
                 "-- CMD [ARGS...]");
    std::exit(2);
}

// Fork + exec CMD. The child restores an empty signal mask before execvp so
// it does not inherit the parent's blocked TERM/INT/HUP/CHLD set; after
// fork() only async-signal-safe calls happen (sigprocmask, execvp, write,
// _exit).
[[nodiscard]] std::expected<pid_t, std::error_code>
spawn(const std::vector<std::string>& cmd) {
    std::vector<char*> argv;
    argv.reserve(cmd.size() + 1);
    for (const auto& arg : cmd) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        return std::unexpected(errno_ec());
    }
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::execvp(argv[0], argv.data());
        constexpr char msg[] = "pmon: error: exec failed\n";
        [[maybe_unused]] auto n = ::write(STDERR_FILENO, msg, sizeof msg - 1);
        ::_exit(127);
    }
    std::println("pmon: started pid {}", pid);
    return pid;
}

struct ChildExit {
    int status = 0;  // exit status when !signaled
    int signo = 0;   // terminating signal when signaled
    bool signaled = false;
};

[[nodiscard]] ChildExit decode(int wstatus) {
    if (WIFSIGNALED(wstatus)) {
        return {.status = 0, .signo = WTERMSIG(wstatus), .signaled = true};
    }
    return {.status = WEXITSTATUS(wstatus), .signo = 0, .signaled = false};
}

void report(pid_t pid, const ChildExit& ce) {
    if (ce.signaled) {
        std::println("pmon: child {} killed signal={}", pid, ce.signo);
    } else {
        std::println("pmon: child {} exited status={}", pid, ce.status);
    }
}

// Blocking reap that tolerates EINTR (cannot fire while the supervisor's
// mask blocks everything we watch, but `run` reaps with the default mask).
[[nodiscard]] std::expected<ChildExit, std::error_code> reap_blocking(pid_t pid) {
    int wstatus = 0;
    for (;;) {
        if (::waitpid(pid, &wstatus, 0) >= 0) {
            return decode(wstatus);
        }
        if (errno != EINTR) {
            return std::unexpected(errno_ec());
        }
    }
}

int run_once(const std::vector<std::string>& cmd) {
    const auto pid = spawn(cmd);
    if (!pid) {
        die("fork", pid.error());
    }
    const auto ce = reap_blocking(*pid);
    if (!ce) {
        die("waitpid", ce.error());
    }
    report(*pid, *ce);
    return ce->signaled ? 128 + ce->signo : ce->status;
}

struct SuperviseOpts {
    int max_restarts = 5;
    long backoff_ms = 100;
    std::vector<std::string> cmd;
};

int supervise(const SuperviseOpts& opts) {
    // Block the signals we care about, then receive them as fd reads.
    sigset_t mask;
    sigemptyset(&mask);
    for (const int signo : {SIGTERM, SIGINT, SIGHUP, SIGCHLD}) {
        sigaddset(&mask, signo);
    }
    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
        die("sigprocmask", errno_ec());
    }
    UniqueFd sfd(::signalfd(-1, &mask, SFD_CLOEXEC));
    if (!sfd.valid()) {
        die("signalfd", errno_ec());
    }

    auto start = [&] {
        const auto pid = spawn(opts.cmd);
        if (!pid) {
            die("fork", pid.error());
        }
        return *pid;
    };
    // SIGTERM the child and reap it. Used by shutdown and reload paths.
    auto stop_child = [&](pid_t pid) {
        if (::kill(pid, SIGTERM) == 0 || errno == ESRCH) {
            if (const auto ce = reap_blocking(pid); !ce) {
                die("waitpid", ce.error());
            }
        }
    };

    pid_t child = start();
    bool running = true;  // false: no child alive, waiting out a backoff
    int restarts = 0;
    long backoff = opts.backoff_ms;
    steady_clock::time_point deadline{};

    for (;;) {
        int timeout = -1;
        if (!running) {
            const auto left = deadline - steady_clock::now();
            timeout =
                left <= 0ns
                    ? 0
                    : static_cast<int>(
                          duration_cast<milliseconds>(left).count()) +
                          1;
        }
        pollfd pfd{.fd = sfd.get(), .events = POLLIN, .revents = 0};
        const int nready = ::poll(&pfd, 1, timeout);
        if (nready < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("poll", errno_ec());
        }
        if (nready == 0) {
            // Backoff elapsed: bring the child back.
            child = start();
            running = true;
            continue;
        }

        signalfd_siginfo si{};
        const ssize_t nread = ::read(sfd.get(), &si, sizeof si);
        if (nread != static_cast<ssize_t>(sizeof si)) {
            die("read signalfd", errno_ec());
        }

        switch (si.ssi_signo) {
        case SIGTERM:
        case SIGINT: {
            if (running) {
                stop_child(child);
            }
            std::println("pmon: shutting down ({})",
                         si.ssi_signo == SIGTERM ? "SIGTERM" : "SIGINT");
            return 0;
        }
        case SIGHUP: {
            std::println("pmon: reload requested");
            if (running) {
                stop_child(child);
            }
            restarts = 0;
            backoff = opts.backoff_ms;
            child = start();
            running = true;
            break;
        }
        case SIGCHLD: {
            int wstatus = 0;
            const pid_t reaped = ::waitpid(child, &wstatus, WNOHANG);
            if (!running || reaped != child) {
                break;  // stale notification (e.g. already reaped on reload)
            }
            const ChildExit ce = decode(wstatus);
            report(child, ce);
            if (!ce.signaled && ce.status == 0) {
                return 0;
            }
            if (restarts >= opts.max_restarts) {
                std::println("pmon: giving up after {} restarts",
                             opts.max_restarts);
                return 1;
            }
            ++restarts;
            std::println("pmon: restart #{} (backoff {}ms)", restarts, backoff);
            deadline = steady_clock::now() + milliseconds(backoff);
            backoff *= 2;
            running = false;
            break;
        }
        default:
            break;
        }
    }
}

// Parse "[flags] -- CMD [ARGS...]" for supervise.
[[nodiscard]] std::expected<SuperviseOpts, std::string>
parse_supervise(const std::vector<std::string>& args) {
    SuperviseOpts opts;
    std::size_t i = 0;
    auto numeric_flag = [&](const char* name, long min,
                            long& out) -> std::expected<void, std::string> {
        if (i + 1 >= args.size()) {
            return std::unexpected(std::string(name) + " needs a value");
        }
        const std::string& value = args[++i];
        char* end = nullptr;
        errno = 0;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (errno != 0 || end == value.c_str() || *end != '\0' ||
            parsed < min) {
            return std::unexpected("bad value for " + std::string(name));
        }
        out = parsed;
        return {};
    };
    while (i < args.size()) {
        const std::string& arg = args[i];
        if (arg == "--") {
            ++i;
            break;
        }
        if (arg == "--max-restarts") {
            long v = 0;
            if (auto r = numeric_flag("--max-restarts", 0, v); !r) {
                return std::unexpected(r.error());
            }
            opts.max_restarts = static_cast<int>(v);
        } else if (arg == "--backoff-ms") {
            long v = 0;
            if (auto r = numeric_flag("--backoff-ms", 1, v); !r) {
                return std::unexpected(r.error());
            }
            opts.backoff_ms = v;
        } else {
            return std::unexpected("unknown flag " + arg);
        }
        ++i;
    }
    for (; i < args.size(); ++i) {
        opts.cmd.push_back(args[i]);
    }
    if (opts.cmd.empty()) {
        return std::unexpected("no command after --");
    }
    return opts;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // line-buffered even into a file

    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage();
    }
    const std::string& sub = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());

    if (sub == "run") {
        if (rest.size() < 2 || rest[0] != "--") {
            usage();
        }
        return run_once({rest.begin() + 1, rest.end()});
    }
    if (sub == "supervise") {
        const auto opts = parse_supervise(rest);
        if (!opts) {
            std::println(stderr, "pmon: error: {}", opts.error());
            usage();
        }
        return supervise(*opts);
    }
    usage();
}
