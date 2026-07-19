// pmon v4 — the process supervisor becomes a log pipeline.
//
// v0 (`run`) spawns a command, waits, and mirrors its exit status.
// v1 (`supervise --engine sigchld`) restarts a crashing child via SIGCHLD
//     delivered as readable data through a signalfd.
// v2 (`supervise --engine pidfd`, the DEFAULT) supervises through
//     pidfd_open(2)/poll(2)/waitid(P_PIDFD) — no pid-reuse race.
// v4 (this chapter) keeps both engines and adds pipes:
//     * supervise captures the child's stdout AND stderr through two
//       pipe(2)s into a log file as "[out] ..."/"[err] ..." lines — the two
//       pipe read ends simply join the fd set each engine already polls;
//     * `tail --log F --fifo PATH` creates a FIFO and relays log bytes into
//       whatever reader attaches — splice(2) fast path (file -> pipe,
//       kernel-side), read/write fallback; SIGPIPE ignored, EPIPE reported
//       as "pmon: tail reader detached" and survived, nothing lost.

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

constexpr std::size_t kChunk = 64 * 1024;
constexpr int kPollMs = 50;

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
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
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

class FileActions {
public:
    FileActions() { ::posix_spawn_file_actions_init(&fa_); }
    FileActions(const FileActions&) = delete;
    FileActions& operator=(const FileActions&) = delete;
    ~FileActions() { ::posix_spawn_file_actions_destroy(&fa_); }
    [[nodiscard]] posix_spawn_file_actions_t* get() noexcept { return &fa_; }

private:
    posix_spawn_file_actions_t fa_{};
};

// ---------------------------------------------------------------------------
// Spawning and status plumbing shared by every subcommand.
// ---------------------------------------------------------------------------

// The supervisor blocks signals for its signalfd; the child must NOT inherit
// that mask (a supervised `sleep` with SIGTERM blocked would never stop), so
// posix_spawn resets the child's mask to empty. When out/err are valid fds
// they become the child's stdout/stderr (the v4 capture pipes).
[[nodiscard]] std::expected<pid_t, std::error_code>
spawn_child(const std::vector<std::string>& cmd, const Fd& out, const Fd& err) {
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

    FileActions fa;
    if (out.valid()) {
        ::posix_spawn_file_actions_adddup2(fa.get(), out.get(), STDOUT_FILENO);
    }
    if (err.valid()) {
        ::posix_spawn_file_actions_adddup2(fa.get(), err.get(), STDERR_FILENO);
    }

    pid_t pid{};
    const int rc = ::posix_spawnp(&pid, argv[0], fa.get(), attrs.get(), argv.data(), environ);
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
// v0: run — spawn, wait, mirror (stdio inherited, no capture).
// ---------------------------------------------------------------------------

int cmd_run(const std::vector<std::string>& cmd) {
    const auto pid = spawn_child(cmd, Fd{}, Fd{});
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
// v4 capture: two pipes, line-buffered into the log with [out]/[err] tags.
// ---------------------------------------------------------------------------

// Accumulates one stream's bytes and appends complete lines to the log fd as
// "[pfx] line\n". Line-buffered: each completed line is written immediately.
class LineRelay {
public:
    LineRelay(std::string_view pfx, const Fd& log) : pfx_{pfx}, log_{log} {}

    [[nodiscard]] std::expected<void, std::error_code> feed(std::string_view chunk) {
        buf_.append(chunk);
        std::size_t start = 0;
        for (std::size_t nl = buf_.find('\n', start); nl != std::string::npos;
             nl = buf_.find('\n', start)) {
            if (auto w = emit(std::string_view{buf_}.substr(start, nl - start)); !w) {
                return w;
            }
            start = nl + 1;
        }
        buf_.erase(0, start);
        if (buf_.size() > kChunk) {  // bound the partial-line buffer
            auto w = emit(buf_);
            buf_.clear();
            return w;
        }
        return {};
    }

    // Stream closed: a trailing partial line still gets logged, with '\n'.
    [[nodiscard]] std::expected<void, std::error_code> flush() {
        if (buf_.empty()) {
            return {};
        }
        auto w = emit(buf_);
        buf_.clear();
        return w;
    }

private:
    [[nodiscard]] std::expected<void, std::error_code> emit(std::string_view line) {
        const std::string rec = std::format("[{}] {}\n", pfx_, line);
        std::size_t off = 0;
        while (off < rec.size()) {
            const ssize_t n = ::write(log_.get(), rec.data() + off, rec.size() - off);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(last_error());
            }
            off += static_cast<std::size_t>(n);
        }
        return {};
    }

    std::string pfx_;
    const Fd& log_;
    std::string buf_;
};

// One child's capture pipes and their relays.
struct Capture {
    Fd out_r;
    Fd err_r;
    LineRelay out_relay;
    LineRelay err_relay;
    bool out_open = true;
    bool err_open = true;

    explicit Capture(const Fd& log)
        : out_relay{"out", log}, err_relay{"err", log} {}

    // One readiness-driven read on a pipe; EOF closes it and flushes.
    [[nodiscard]] std::expected<void, std::error_code>
    drain_once(Fd& fd, LineRelay& relay, bool& open) {
        std::array<char, kChunk> buf;
        const ssize_t n = ::read(fd.get(), buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                return {};
            }
            return std::unexpected(last_error());
        }
        if (n == 0) {
            open = false;
            fd.reset();
            return relay.flush();
        }
        return relay.feed(std::string_view{buf.data(), static_cast<std::size_t>(n)});
    }

    // The child is reaped: blocking-read both pipes to EOF so every logged
    // byte lands before the exit is reported.
    [[nodiscard]] std::expected<void, std::error_code> drain_to_eof() {
        while (out_open) {
            if (auto r = drain_once(out_r, out_relay, out_open); !r) {
                return r;
            }
        }
        while (err_open) {
            if (auto r = drain_once(err_r, err_relay, err_open); !r) {
                return r;
            }
        }
        return {};
    }
};

// ---------------------------------------------------------------------------
// v1/v2 engines with the v4 pipes joined into the same poll set.
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

// One poll loop for both engines: the capture pipes, the signalfd, and (pidfd
// engine only) the pidfd. A closed pipe leaves the set as fd=-1, which
// poll(2) ignores.
[[nodiscard]] std::expected<Outcome, std::error_code>
wait_child(Engine engine, pid_t pid, const Fd& pidfd, const Fd& sigfd,
           steady_clock::time_point deadline, Capture& cap) {
    for (;;) {
        std::array<pollfd, 4> fds{{
            {.fd = sigfd.get(), .events = POLLIN, .revents = 0},
            {.fd = cap.out_open ? cap.out_r.get() : -1, .events = POLLIN, .revents = 0},
            {.fd = cap.err_open ? cap.err_r.get() : -1, .events = POLLIN, .revents = 0},
            {.fd = engine == Engine::pidfd ? pidfd.get() : -1,
             .events = POLLIN,
             .revents = 0},
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
        if (fds[1].revents != 0) {
            if (auto r = cap.drain_once(cap.out_r, cap.out_relay, cap.out_open); !r) {
                return std::unexpected(r.error());
            }
        }
        if (fds[2].revents != 0) {
            if (auto r = cap.drain_once(cap.err_r, cap.err_relay, cap.err_open); !r) {
                return std::unexpected(r.error());
            }
        }
        if ((fds[0].revents & POLLIN) != 0) {
            signalfd_siginfo info{};
            std::ignore = ::read(sigfd.get(), &info, sizeof info);
            if (info.ssi_signo != SIGCHLD) {
                return Stop::signal;
            }
            // sigchld engine: WNOHANG-reap; coalesced/stale SIGCHLD is benign.
            siginfo_t si{};
            si.si_pid = 0;
            if (::waitid(P_PID, static_cast<id_t>(pid), &si, WEXITED | WNOHANG) != 0) {
                return std::unexpected(last_error());
            }
            if (si.si_pid == pid) {
                return Outcome{status_from(si)};
            }
        }
        if (engine == Engine::pidfd && fds[3].revents != 0) {
            return reap_pidfd(pidfd).transform([](ChildStatus st) { return Outcome{st}; });
        }
    }
}

// Ask the child to terminate and reap it (stop paths).
[[nodiscard]] std::expected<ChildStatus, std::error_code>
term_and_reap(Engine engine, pid_t pid, const Fd& pidfd) {
    if (engine == Engine::pidfd) {
        std::ignore = ::syscall(SYS_pidfd_send_signal, pidfd.get(), SIGTERM, nullptr, 0);
        return reap_pidfd(pidfd);
    }
    std::ignore = ::kill(pid, SIGTERM);
    siginfo_t si{};
    while (::waitid(P_PID, static_cast<id_t>(pid), &si, WEXITED) != 0) {
        if (errno != EINTR) {
            return std::unexpected(last_error());
        }
    }
    return status_from(si);
}

int cmd_supervise(Engine engine, int max_restarts, int timeout_ms,
                  const std::string& log_path, const std::vector<std::string>& cmd) {
    auto log = checked(::open(log_path.c_str(),
                              O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644));
    if (!log) {
        std::println(stderr, "pmon: open {}: {}", log_path, log.error().message());
        return 1;
    }
    auto sigfd = engine == Engine::pidfd
                     ? make_signalfd({SIGINT, SIGTERM})
                     : make_signalfd({SIGCHLD, SIGINT, SIGTERM});
    if (!sigfd) {
        std::println(stderr, "pmon: signalfd: {}", sigfd.error().message());
        return 1;
    }
    const auto deadline = steady_clock::now() + milliseconds{timeout_ms};
    int restarts = 0;
    for (;;) {
        Capture cap{*log};
        std::array<int, 2> outp{}, errp{};
        if (::pipe2(outp.data(), O_CLOEXEC) != 0 || ::pipe2(errp.data(), O_CLOEXEC) != 0) {
            std::println(stderr, "pmon: pipe2: {}", last_error().message());
            return 1;
        }
        cap.out_r = Fd{outp[0]};
        Fd out_w{outp[1]};
        cap.err_r = Fd{errp[0]};
        Fd err_w{errp[1]};

        const auto pid = spawn_child(cmd, out_w, err_w);
        if (!pid) {
            std::println(stderr, "pmon: spawn: {}: {}", cmd[0], pid.error().message());
            return 1;
        }
        out_w.reset();  // parent's copies of the write ends must close,
        err_w.reset();  // or EOF never arrives

        Fd pidfd;
        if (engine == Engine::pidfd) {
            auto pf = checked(static_cast<int>(::syscall(SYS_pidfd_open, *pid, 0)));
            if (!pf) {
                std::println(stderr, "pmon: pidfd_open: {}", pf.error().message());
                return 1;
            }
            pidfd = std::move(*pf);
            std::println(stderr, "pmon: engine=pidfd child={} pidfd={}", *pid, pidfd.get());
        } else {
            std::println(stderr, "pmon: engine=sigchld child={}", *pid);
        }

        auto outcome = wait_child(engine, *pid, pidfd, *sigfd, deadline, cap);
        if (!outcome) {
            std::println(stderr, "pmon: wait: {}", outcome.error().message());
            return 1;
        }

        if (const auto* stop = std::get_if<Stop>(&*outcome)) {
            const auto st = term_and_reap(engine, *pid, pidfd);
            if (!st) {
                std::println(stderr, "pmon: waitid: {}", st.error().message());
                return 1;
            }
            if (auto r = cap.drain_to_eof(); !r) {
                std::println(stderr, "pmon: relay: {}", r.error().message());
                return 1;
            }
            std::ignore = report_exit(*pid, *st);
            std::println("pmon: exiting ({})", *stop == Stop::timeout ? "timeout" : "signal");
            std::fflush(stdout);
            return 0;
        }

        // Reaped: finish the log before reporting, then apply the policy.
        if (auto r = cap.drain_to_eof(); !r) {
            std::println(stderr, "pmon: relay: {}", r.error().message());
            return 1;
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
// v4: tail — follow the log, relay into a FIFO, survive reader churn.
// ---------------------------------------------------------------------------

volatile sig_atomic_t g_stop = 0;

extern "C" void on_stop(int) {
    g_stop = 1;
}

void sleep_poll() {
    const timespec ts{.tv_sec = 0, .tv_nsec = kPollMs * 1'000'000};
    ::nanosleep(&ts, nullptr);  // EINTR just returns early; callers recheck g_stop
}

// Open the FIFO for writing without blocking forever: O_NONBLOCK turns
// "no reader yet" into ENXIO, which we poll. Once a reader is attached the
// fd goes back to blocking mode for the relay.
[[nodiscard]] std::expected<Fd, std::error_code> open_writer(const std::string& fifo) {
    while (g_stop == 0) {
        auto fd = checked(::open(fifo.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC));
        if (fd) {
            const int fl = ::fcntl(fd->get(), F_GETFL);
            if (fl < 0 || ::fcntl(fd->get(), F_SETFL, fl & ~O_NONBLOCK) < 0) {
                return std::unexpected(last_error());
            }
            return fd;
        }
        if (fd.error().value() != ENXIO && fd.error().value() != EINTR) {
            return std::unexpected(fd.error());
        }
        sleep_poll();
    }
    return Fd{};  // stop requested: invalid fd sentinel
}

enum class Relay : std::uint8_t { moved, idle, detached, no_splice };

// splice(2) fast path: move log bytes kernel-side into the pipe. The file
// offset only advances on success, so a detached reader (EPIPE) loses
// nothing — the same bytes are respliced for the next reader.
[[nodiscard]] std::expected<Relay, std::error_code>
relay_splice(const Fd& log, const Fd& w) {
    const ssize_t n = ::splice(log.get(), nullptr, w.get(), nullptr, kChunk, 0);
    if (n > 0) {
        return Relay::moved;
    }
    if (n == 0) {
        return Relay::idle;
    }
    switch (errno) {
        case EPIPE:
            return Relay::detached;
        case EINTR:
        case EAGAIN:
            return Relay::idle;
        case EINVAL:
        case ENOSYS:
            return Relay::no_splice;  // fs without splice support: fall back
        default:
            return std::unexpected(last_error());
    }
}

// read/write fallback with the same no-loss contract: on EPIPE the file
// offset is rewound to the first unwritten byte.
[[nodiscard]] std::expected<Relay, std::error_code>
relay_rw(const Fd& log, const Fd& w) {
    const off_t before = ::lseek(log.get(), 0, SEEK_CUR);
    std::array<char, kChunk> buf;
    const ssize_t n = ::read(log.get(), buf.data(), buf.size());
    if (n < 0) {
        if (errno == EINTR) {
            return Relay::idle;
        }
        return std::unexpected(last_error());
    }
    if (n == 0) {
        return Relay::idle;
    }
    ssize_t written = 0;
    while (written < n) {
        const ssize_t k = ::write(w.get(), buf.data() + written,
                                  static_cast<std::size_t>(n - written));
        if (k < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int saved = errno;
            ::lseek(log.get(), before + written, SEEK_SET);
            if (saved == EPIPE) {
                return Relay::detached;
            }
            return std::unexpected(std::error_code{saved, std::system_category()});
        }
        written += k;
    }
    return Relay::moved;
}

int cmd_tail(const std::string& log_path, const std::string& fifo) {
    struct sigaction ign{};
    ign.sa_handler = SIG_IGN;  // EPIPE stays an error value, never fatal
    ::sigaction(SIGPIPE, &ign, nullptr);
    struct sigaction stop{};
    stop.sa_handler = on_stop;  // no SA_RESTART: blocking calls return EINTR
    ::sigaction(SIGINT, &stop, nullptr);
    ::sigaction(SIGTERM, &stop, nullptr);

    if (::mkfifo(fifo.c_str(), 0644) != 0) {
        if (errno != EEXIST) {
            std::println(stderr, "pmon: mkfifo {}: {}", fifo, last_error().message());
            return 1;
        }
        struct stat st{};
        if (::stat(fifo.c_str(), &st) != 0 || !S_ISFIFO(st.st_mode)) {
            std::println(stderr, "pmon: {} exists and is not a FIFO", fifo);
            return 1;
        }
    }
    auto log = checked(::open(log_path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!log) {
        std::println(stderr, "pmon: open {}: {}", log_path, log.error().message());
        return 1;
    }
    std::println(stderr, "pmon: tail ready (fifo {})", fifo);

    bool use_splice = true;
    while (g_stop == 0) {
        auto w = open_writer(fifo);
        if (!w) {
            std::println(stderr, "pmon: open {}: {}", fifo, w.error().message());
            return 1;
        }
        if (!w->valid()) {
            break;  // stop requested while waiting for a reader
        }
        bool attached = true;
        while (g_stop == 0 && attached) {
            const auto r = use_splice ? relay_splice(*log, *w) : relay_rw(*log, *w);
            if (!r) {
                std::println(stderr, "pmon: relay: {}", r.error().message());
                return 1;
            }
            switch (*r) {
                case Relay::moved:
                    break;
                case Relay::idle:
                    sleep_poll();
                    break;
                case Relay::no_splice:
                    use_splice = false;
                    break;
                case Relay::detached:
                    std::println("pmon: tail reader detached");
                    std::fflush(stdout);
                    attached = false;
                    break;
            }
        }
    }
    std::println("pmon: exiting (signal)");
    std::fflush(stdout);
    return 0;
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
                 "            [--log FILE] -- CMD [ARGS...]  restart CMD on abnormal exit;");
    std::println(stderr,
                 "                                           capture stdout/stderr into FILE");
    std::println(stderr,
                 "                                           (defaults: pidfd, N=3, T=10000,");
    std::println(stderr,
                 "                                           FILE=pmon.log)");
    std::println(stderr,
                 "  tail --log FILE --fifo PATH              relay appended log lines into a");
    std::println(stderr,
                 "                                           FIFO created at PATH");
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

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args{argv + 1, argv + argc};
    if (args.empty()) {
        return usage();
    }

    if (args[0] == "tail") {
        std::string log_path, fifo;
        std::size_t i = 1;
        while (i + 1 < args.size()) {
            if (args[i] == "--log") {
                log_path = args[i + 1];
            } else if (args[i] == "--fifo") {
                fifo = args[i + 1];
            } else {
                return usage();
            }
            i += 2;
        }
        if (i != args.size() || log_path.empty() || fifo.empty()) {
            return usage();
        }
        return cmd_tail(log_path, fifo);
    }

    const auto sep = std::ranges::find(args, std::string_view{"--"});
    if (sep == args.end() || sep + 1 == args.end()) {
        return usage();  // run and supervise need `-- CMD [ARGS...]`
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
        std::string log_path = "pmon.log";
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
            } else if (flag == "--log") {
                log_path = value;
            } else {
                return usage();
            }
        }
        return cmd_supervise(engine, max_restarts, timeout_ms, log_path, cmd);
    }
    return usage();
}
