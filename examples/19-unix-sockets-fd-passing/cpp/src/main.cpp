// pmon v5 — a process supervisor with a UNIX-socket control plane.
//
// `pmon supervise` forks/execs a child command (its own process group,
// stdout+stderr appended to a log file), restarts it 300 ms after every
// exit, and serves a SOCK_STREAM control socket. `pmon pmctl` is the client
// side of the same binary:
//
//   status  -> "child=<pid> uptime=<s>s restarts=<n>"
//   stop    -> "stopping", then the supervisor tears down and exits 0
//   logfd   -> the supervisor passes its OPEN log file descriptor through an
//              SCM_RIGHTS control message; pmctl reads the last 3 lines
//              straight off the received fd ("via-fd: ..."), never touching
//              the path. That is the point of the chapter: file descriptors
//              are capabilities you can hand across a UNIX socket.
//
// Every accepted connection is identified with SO_PEERCRED before a single
// command byte is trusted. The cmsg plumbing here is done by hand with
// sendmsg(2)/recvmsg(2) — CMSG_FIRSTHDR and friends, no wrapper library.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using std::chrono::duration_cast;
using std::chrono::seconds;
using std::chrono::steady_clock;

constexpr auto kRestartBackoff = 300ms;
constexpr std::size_t kMaxLogRead = 1u << 20;  // pmctl reads at most 1 MiB of log

[[nodiscard]] std::string errno_text() {
    return std::error_code{errno, std::system_category()}.message();
}

template <typename T>
using Result = std::expected<T, std::string>;

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
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

private:
    int fd_ = -1;
};

// ---------------------------------------------------------------------------
// Socket + log plumbing, each step a std::expected
// ---------------------------------------------------------------------------

Result<Fd> open_log(const std::string& path) {
    // O_RDWR, not O_WRONLY: the fd handed out via SCM_RIGHTS shares this open
    // file description, and pmctl must be able to read from it.
    Fd fd{::open(path.c_str(), O_CREAT | O_RDWR | O_APPEND | O_CLOEXEC, 0644)};
    if (!fd.valid()) {
        return std::unexpected(std::format("open {}: {}", path, errno_text()));
    }
    return fd;
}

Result<sockaddr_un> unix_addr(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        return std::unexpected(std::format("{}: socket path too long", path));
    }
    std::ranges::copy(path, addr.sun_path);
    return addr;
}

Result<Fd> listen_ctl(const std::string& path) {
    ::unlink(path.c_str());  // stale socket from a previous run; ENOENT is fine
    Fd fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!fd.valid()) {
        return std::unexpected(std::format("socket: {}", errno_text()));
    }
    auto addr = unix_addr(path);
    if (!addr) {
        return std::unexpected(addr.error());
    }
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&*addr), sizeof(*addr)) < 0) {
        return std::unexpected(std::format("bind {}: {}", path, errno_text()));
    }
    if (::listen(fd.get(), 8) < 0) {
        return std::unexpected(std::format("listen {}: {}", path, errno_text()));
    }
    return fd;
}

Result<Fd> connect_ctl(const std::string& path) {
    Fd fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!fd.valid()) {
        return std::unexpected(std::format("socket: {}", errno_text()));
    }
    auto addr = unix_addr(path);
    if (!addr) {
        return std::unexpected(addr.error());
    }
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&*addr), sizeof(*addr)) < 0) {
        return std::unexpected(std::format("connect {}: {}", path, errno_text()));
    }
    return fd;
}

void write_all(int fd, std::string_view text) {
    std::size_t off = 0;
    while (off < text.size()) {
        ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;  // peer gone; nothing sensible to do with a reply error
        }
        off += static_cast<std::size_t>(n);
    }
}

// One write(2) per line: O_APPEND makes each line an atomic append even while
// the child writes to the same file.
void log_line(int log_fd, std::string_view line) {
    write_all(log_fd, std::format("{}\n", line));
}

// ---------------------------------------------------------------------------
// Child lifecycle
// ---------------------------------------------------------------------------

Result<pid_t> spawn_child(const std::vector<std::string>& argv, int log_fd) {
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) {
        cargv.push_back(const_cast<char*>(a.c_str()));
    }
    cargv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        return std::unexpected(std::format("fork: {}", errno_text()));
    }
    if (pid == 0) {
        ::setpgid(0, 0);  // own process group, so stop can signal the whole tree
        ::dup2(log_fd, STDOUT_FILENO);
        ::dup2(log_fd, STDERR_FILENO);
        ::execvp(cargv[0], cargv.data());
        std::println(stderr, "pmon: exec {}: {}", argv[0], errno_text());  // lands in the log
        ::_exit(127);
    }
    return pid;
}

struct State {
    std::mutex m;
    std::condition_variable cv;
    pid_t child = -1;
    steady_clock::time_point started{};
    int restarts = 0;
    bool stopping = false;
    bool child_done = false;  // reaper has collected the final child
};

[[nodiscard]] int exit_code_of(int wstatus) {
    if (WIFEXITED(wstatus)) {
        return WEXITSTATUS(wstatus);
    }
    if (WIFSIGNALED(wstatus)) {
        return 128 + WTERMSIG(wstatus);
    }
    return 1;
}

// Reaper thread: waitpid the current child; unless we are stopping, back off
// 300 ms and respawn. Ends by reporting child_done through the cv.
void reaper(State& s, const std::vector<std::string>& argv, int log_fd) {
    pid_t pid = 0;
    {
        std::lock_guard lk(s.m);
        pid = s.child;
    }
    auto finish = [&s] {
        std::lock_guard lk(s.m);
        s.child_done = true;
        s.cv.notify_all();
    };
    for (;;) {
        int wstatus = 0;
        if (::waitpid(pid, &wstatus, 0) < 0) {
            if (errno == EINTR) {
                continue;
            }
            finish();
            return;
        }
        {
            std::lock_guard lk(s.m);
            if (s.stopping) {
                s.child_done = true;
                s.cv.notify_all();
                return;
            }
        }
        std::println(stderr, "pmon: child pid={} exited status={}", pid, exit_code_of(wstatus));
        {
            std::unique_lock lk(s.m);
            if (s.cv.wait_for(lk, kRestartBackoff, [&s] { return s.stopping; })) {
                s.child_done = true;
                s.cv.notify_all();
                return;
            }
        }
        auto next = spawn_child(argv, log_fd);
        if (!next) {
            std::println(stderr, "pmon: error: {}", next.error());
            finish();
            return;
        }
        int restarts_now = 0;
        bool stop_now = false;
        {
            std::lock_guard lk(s.m);
            stop_now = s.stopping;
            if (!stop_now) {
                s.child = *next;
                s.started = steady_clock::now();
                restarts_now = ++s.restarts;
            }
        }
        if (stop_now) {  // stop raced with the respawn: undo it
            ::kill(-*next, SIGTERM);
            ::waitpid(*next, nullptr, 0);
            finish();
            return;
        }
        std::println(stderr, "pmon: restart {} child pid={}", restarts_now, *next);
        log_line(log_fd, std::format("pmon: start child pid={}", *next));
        pid = *next;
    }
}

// ---------------------------------------------------------------------------
// SCM_RIGHTS, by hand
// ---------------------------------------------------------------------------

void send_fd(int sock, int fd) {
    char payload[] = "ok\n";
    iovec iov{payload, 3};
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int))]{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    if (::sendmsg(sock, &msg, 0) < 0) {
        std::println(stderr, "pmon: sendmsg: {}", errno_text());
    }
}

Result<Fd> recv_fd(int sock) {
    char payload[16];
    iovec iov{payload, sizeof(payload)};
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int))]{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    ssize_t n = ::recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    if (n < 0) {
        return std::unexpected(std::format("recvmsg: {}", errno_text()));
    }
    for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm != nullptr; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            int fd = -1;
            std::memcpy(&fd, CMSG_DATA(cm), sizeof(int));
            return Fd{fd};
        }
    }
    return std::unexpected("no SCM_RIGHTS control message in reply");
}

// ---------------------------------------------------------------------------
// supervise
// ---------------------------------------------------------------------------

std::string read_command(int conn) {
    std::string cmd;
    while (cmd.size() < 64) {
        char ch = 0;
        ssize_t n = ::read(conn, &ch, 1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0 || ch == '\n') {
            break;
        }
        cmd.push_back(ch);
    }
    return cmd;
}

int cmd_supervise(const std::string& ctl, const std::string& log_path,
                  const std::vector<std::string>& child_argv) {
    auto log = open_log(log_path);
    if (!log) {
        std::println(stderr, "pmon: error: {}", log.error());
        return 1;
    }
    auto lsock = listen_ctl(ctl);
    if (!lsock) {
        std::println(stderr, "pmon: error: {}", lsock.error());
        return 1;
    }
    std::println(stderr, "pmon: listening on {}", ctl);

    auto first = spawn_child(child_argv, log->get());
    if (!first) {
        std::println(stderr, "pmon: error: {}", first.error());
        ::unlink(ctl.c_str());
        return 1;
    }
    std::println(stderr, "pmon: started child pid={}", *first);
    log_line(log->get(), std::format("pmon: start child pid={}", *first));

    State s;
    s.child = *first;
    s.started = steady_clock::now();
    std::jthread reap([&s, &child_argv, log_fd = log->get()] { reaper(s, child_argv, log_fd); });

    for (;;) {
        Fd conn{::accept4(lsock->get(), nullptr, nullptr, SOCK_CLOEXEC)};
        if (!conn.valid()) {
            if (errno == EINTR) {
                continue;
            }
            std::println(stderr, "pmon: error: accept: {}", errno_text());
            return 1;
        }

        ucred cred{};
        socklen_t cred_len = sizeof(cred);
        if (::getsockopt(conn.get(), SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) == 0) {
            std::println(stderr, "pmon: ctl connect uid={} pid={}", cred.uid, cred.pid);
        }

        const std::string cmd = read_command(conn.get());
        if (cmd == "status") {
            std::lock_guard lk(s.m);
            auto up = duration_cast<seconds>(steady_clock::now() - s.started).count();
            write_all(conn.get(),
                      std::format("child={} uptime={}s restarts={}\n", s.child, up, s.restarts));
        } else if (cmd == "logfd") {
            send_fd(conn.get(), log->get());
        } else if (cmd == "stop") {
            write_all(conn.get(), "stopping\n");
            conn.reset();  // deliver the reply before tearing down
            std::println(stderr, "pmon: stopping");
            pid_t child = -1;
            {
                std::lock_guard lk(s.m);
                s.stopping = true;
                child = s.child;
            }
            s.cv.notify_all();
            if (child > 0) {
                ::kill(-child, SIGTERM);  // whole process group; ESRCH is fine
            }
            {
                std::unique_lock lk(s.m);
                s.cv.wait(lk, [&s] { return s.child_done; });
            }
            ::unlink(ctl.c_str());
            return 0;
        } else {
            write_all(conn.get(), "err unknown command\n");
        }
    }
}

// ---------------------------------------------------------------------------
// pmctl
// ---------------------------------------------------------------------------

Result<std::vector<std::string>> log_tail_via_fd(int fd, std::size_t count) {
    std::string data;
    off_t off = 0;
    char buf[4096];
    while (data.size() < kMaxLogRead) {
        ssize_t n = ::pread(fd, buf, sizeof(buf), off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(std::format("pread: {}", errno_text()));
        }
        if (n == 0) {
            break;
        }
        data.append(buf, static_cast<std::size_t>(n));
        off += n;
    }
    if (!data.empty() && data.back() == '\n') {
        data.pop_back();
    }
    std::vector<std::string> lines;
    if (data.empty()) {
        return lines;
    }
    std::size_t pos = 0;
    for (;;) {
        std::size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) {
            lines.emplace_back(data.substr(pos));
            break;
        }
        lines.emplace_back(data.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (lines.size() > count) {
        lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(count));
    }
    return lines;
}

int cmd_pmctl(const std::string& ctl, const std::string& action) {
    auto conn = connect_ctl(ctl);
    if (!conn) {
        std::println(stderr, "pmctl: error: {}", conn.error());
        return 1;
    }
    write_all(conn->get(), std::format("{}\n", action));

    if (action == "logfd") {
        auto fd = recv_fd(conn->get());
        if (!fd) {
            std::println(stderr, "pmctl: error: {}", fd.error());
            return 1;
        }
        auto lines = log_tail_via_fd(fd->get(), 3);
        if (!lines) {
            std::println(stderr, "pmctl: error: {}", lines.error());
            return 1;
        }
        for (const auto& line : *lines) {
            std::println("via-fd: {}", line);
        }
        return 0;
    }

    // status / stop: the supervisor replies with one line and closes.
    std::string reply;
    char buf[256];
    for (;;) {
        ssize_t n = ::read(conn->get(), buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        reply.append(buf, static_cast<std::size_t>(n));
    }
    while (!reply.empty() && reply.back() == '\n') {
        reply.pop_back();
    }
    if (reply.empty()) {
        std::println(stderr, "pmctl: error: empty reply from supervisor");
        return 1;
    }
    if (reply.starts_with("err ")) {
        std::println(stderr, "pmctl: error: {}", reply.substr(4));
        return 1;
    }
    std::println("{}", reply);
    return 0;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

int usage() {
    std::println(stderr, "usage: pmon supervise --ctl SOCK --log FILE -- CMD [ARG...]");
    std::println(stderr, "       pmon pmctl --ctl SOCK <status|stop|logfd>");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        return usage();
    }

    if (args[0] == "supervise") {
        std::string ctl;
        std::string log;
        std::vector<std::string> child;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--ctl" && i + 1 < args.size()) {
                ctl = args[++i];
            } else if (args[i] == "--log" && i + 1 < args.size()) {
                log = args[++i];
            } else if (args[i] == "--") {
                child.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
                break;
            } else {
                return usage();
            }
        }
        if (ctl.empty() || log.empty() || child.empty()) {
            return usage();
        }
        return cmd_supervise(ctl, log, child);
    }

    if (args[0] == "pmctl") {
        std::string ctl;
        std::string action;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--ctl" && i + 1 < args.size()) {
                ctl = args[++i];
            } else if (action.empty()) {
                action = args[i];
            } else {
                return usage();
            }
        }
        if (ctl.empty() || (action != "status" && action != "stop" && action != "logfd")) {
            return usage();
        }
        return cmd_pmctl(ctl, action);
    }

    return usage();
}
