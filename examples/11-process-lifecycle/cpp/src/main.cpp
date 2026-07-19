// pmon run -- CMD [ARGS...] — minimal process monitor for the process
// lifecycle chapter: fork(2) + execvp(3) the command, waitpid(2) for it,
// then report its fate and resource usage.
//
//   pmon: pid <p> exited status <s>              child exited normally
//   pmon: pid <p> killed by signal <n> (<NAME>)  child died on a signal
//   pmon: rusage maxrss=<kb>KB user=<s>.<ms>s sys=<s>.<ms>s wall=<ms>ms
//
// pmon's own exit code mirrors the child: the exit status, or 128+signal.
// If exec itself fails, stderr gets "pmon: exec <cmd>: <reason>" and pmon
// exits 127 with no report lines (there was no child run to report on).
//
// The chapter discusses posix_spawn(3); fork/exec is used here because the
// two-step model — and the CLOEXEC self-pipe that reports exec failure back
// to the parent — is exactly what the chapter teaches.

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <expected>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::string_view_literals;

[[nodiscard]] std::error_code last_error() {
    return {errno, std::system_category()};
}

// Move-only RAII owner of a file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_{fd} {}
    ~Fd() { reset(); }
    Fd(Fd&& other) noexcept : fd_{other.release()} {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.release();
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    [[nodiscard]] int get() const { return fd_; }
    int release() { return std::exchange(fd_, -1); }
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

// Self-pipe for exec-failure reporting: both ends O_CLOEXEC, so a successful
// exec closes the child's copy and the parent reads EOF.
struct ExecPipe {
    Fd read_end;
    Fd write_end;
};

[[nodiscard]] std::expected<ExecPipe, std::error_code> make_exec_pipe() {
    std::array<int, 2> fds{};
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
        return std::unexpected(last_error());
    }
    return ExecPipe{Fd{fds[0]}, Fd{fds[1]}};
}

[[nodiscard]] std::expected<int, std::error_code> wait_child(pid_t pid) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return std::unexpected(last_error());
        }
    }
    return status;
}

[[nodiscard]] std::expected<rusage, std::error_code> children_rusage() {
    rusage ru{};
    if (::getrusage(RUSAGE_CHILDREN, &ru) != 0) {
        return std::unexpected(last_error());
    }
    return ru;
}

// Names as printed in "killed by signal <n> (<NAME>)". A hand-rolled table
// (identical across the three implementations) rather than strsignal(3),
// whose text is locale-dependent prose ("Terminated").
[[nodiscard]] std::string signal_name(int sig) {
    static constexpr std::array<std::string_view, 32> names{
        "",       "HUP",  "INT",  "QUIT", "ILL",  "TRAP", "ABRT",   "BUS",
        "FPE",    "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM",   "TERM",
        "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU",   "URG",
        "XCPU",   "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR",   "SYS"};
    if (sig >= 1 && sig < static_cast<int>(names.size())) {
        return std::string{names[sig]};
    }
    return std::format("SIG{}", sig);
}

// "<sec>.<ms>s" from a timeval, e.g. "0.004s".
[[nodiscard]] std::string format_cpu(const timeval& tv) {
    return std::format("{}.{:03}s", tv.tv_sec, tv.tv_usec / 1000);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4 || argv[1] != "run"sv || argv[2] != "--"sv) {
        std::println(stderr, "usage: pmon run -- CMD [ARGS...]");
        return 2;
    }
    char** child_argv = argv + 3; // already NULL-terminated by the C runtime

    auto pipe = make_exec_pipe();
    if (!pipe) {
        std::println(stderr, "pmon: pipe2: {}", pipe.error().message());
        return 1;
    }

    const auto start = std::chrono::steady_clock::now();
    const pid_t pid = ::fork();
    if (pid < 0) {
        std::println(stderr, "pmon: fork: {}", last_error().message());
        return 1;
    }

    if (pid == 0) {
        // Child. On exec success the O_CLOEXEC pipe closes itself; on failure
        // ship errno to the parent and die with the shell's 127 convention.
        pipe->read_end.reset();
        ::execvp(child_argv[0], child_argv);
        const int exec_errno = errno;
        (void)::write(pipe->write_end.get(), &exec_errno, sizeof exec_errno);
        _exit(127);
    }

    // Parent: drop our write end so read() returns the moment exec resolves.
    pipe->write_end.reset();
    int exec_errno = 0;
    ssize_t nread = 0;
    do {
        nread = ::read(pipe->read_end.get(), &exec_errno, sizeof exec_errno);
    } while (nread < 0 && errno == EINTR);

    const auto status = wait_child(pid);
    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (!status) {
        std::println(stderr, "pmon: waitpid: {}", status.error().message());
        return 1;
    }

    if (nread == static_cast<ssize_t>(sizeof exec_errno)) {
        const std::error_code ec{exec_errno, std::system_category()};
        std::println(stderr, "pmon: exec {}: {}", child_argv[0], ec.message());
        return 127;
    }

    const auto ru = children_rusage();
    if (!ru) {
        std::println(stderr, "pmon: getrusage: {}", ru.error().message());
        return 1;
    }

    int code = 1;
    if (WIFEXITED(*status)) {
        code = WEXITSTATUS(*status);
        std::println("pmon: pid {} exited status {}", pid, code);
    } else if (WIFSIGNALED(*status)) {
        const int sig = WTERMSIG(*status);
        code = 128 + sig;
        std::println("pmon: pid {} killed by signal {} ({})", pid, sig, signal_name(sig));
    } else {
        std::println(stderr, "pmon: unexpected wait status {:#x}", *status);
        return 1;
    }

    std::println("pmon: rusage maxrss={}KB user={} sys={} wall={}ms", ru->ru_maxrss,
                 format_cpu(ru->ru_utime), format_cpu(ru->ru_stime), wall.count());
    return code;
}
