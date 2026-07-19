// app — a tiny PID-1 container entrypoint (chapter 34: our programs in
// containers).
//
//   app serve   run as the container's PID 1: reap zombies, forward SIGTERM
//               to the supervised worker, report the REAL container limits.
//   app naive   the trap: no signal handling installed at all. A process
//               that is PID 1 of its own PID namespace (i.e. the container's
//               entrypoint) is special-cased by the kernel: any signal for
//               which it has installed no explicit handler is dropped
//               instead of running that signal's default action. Only
//               SIGKILL (and SIGSTOP) still work. `naive` never calls
//               sigaction for anything, so `podman stop` cannot end it
//               gracefully -- the engine has to wait out the stop timeout
//               and fall back to SIGKILL.
//
// Container-aware resource detection: hardware_concurrency() is
// sysconf(_SC_NPROCESSORS_ONLN) under the hood -- the HOST's cpu count. It
// never reads a cgroup, so under `podman run --cpus=2` this still prints the
// host's full core count. That mismatch against Go's GOMAXPROCS and Rust's
// available_parallelism() (both cgroup-aware) is the whole point of this
// example: raw C++ threading primitives do not know they are in a
// container.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <poll.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::error_code errno_ec() {
    return {errno, std::generic_category()};
}

[[noreturn]] void die(std::string_view what, std::error_code ec) {
    std::println(stderr, "app: error: {}: {}", what, ec.message());
    std::exit(1);
}

[[noreturn]] void usage() {
    std::println(stderr, "usage: app serve|naive");
    std::exit(2);
}

// Read a whole file into a string, trailing whitespace trimmed. Empty
// string if the file cannot be opened (never fatal: cgroup files may be
// absent on unusual hosts, and the caller falls back to "unknown").
std::string read_trim(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() &&
           (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

// The cgroup v2 unified path this process is itself in: /proc/self/cgroup
// has exactly one line for the unified hierarchy, "0::<path>". Falls back
// to root if the file is missing (e.g. a non-cgroup-v2 host).
std::string own_cgroup_path() {
    std::ifstream f("/proc/self/cgroup");
    std::string line;
    while (f && std::getline(f, line)) {
        if (line.rfind("0::", 0) == 0) {
            return line.substr(3);
        }
    }
    return "/";
}

// Read a cgroup v2 controller file for THIS process's own cgroup. Inside a
// container the cgroup namespace already makes "/sys/fs/cgroup" the
// container's own slice, so the root-relative fallback covers that case too.
std::string read_own_cgroup_file(std::string_view name) {
    static const std::string base = "/sys/fs/cgroup" + own_cgroup_path();
    std::string v = read_trim(base + "/" + std::string(name));
    if (!v.empty()) {
        return v;
    }
    return read_trim("/sys/fs/cgroup/" + std::string(name));
}

// "max" or "<quota>/<period>", derived from cpu.max's raw
// "max 100000" / "200000 100000" content.
std::string cpu_max_display() {
    const std::string raw = read_own_cgroup_file("cpu.max");
    if (raw.empty()) {
        return "unknown";
    }
    const auto sp = raw.find(' ');
    if (sp == std::string::npos) {
        return raw;
    }
    const std::string quota = raw.substr(0, sp);
    if (quota == "max") {
        return "max";
    }
    return quota + "/" + raw.substr(sp + 1);
}

std::string mem_max_display() {
    const std::string raw = read_own_cgroup_file("memory.max");
    return raw.empty() ? "unknown" : raw;
}

void print_container_line() {
    const unsigned n = std::thread::hardware_concurrency(); // THE TRAP: host cpus
    std::println("container: cpu.max={} effective_parallelism={} mem.max={}",
                 cpu_max_display(), n, mem_max_display());
}

// Fork-safe: only async-signal-safe calls between fork() and _exit().
void write_line(std::string_view s) {
    [[maybe_unused]] auto n = ::write(STDOUT_FILENO, s.data(), s.size());
}

// A short-lived "job": forks, prints one line, exits immediately. Left
// unreaped it would sit as a zombie -- reap_all() in the serve loop is what
// keeps these from piling up.
void spawn_job(int seq) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return; // best-effort; a failed fork here is not fatal
    }
    if (pid == 0) {
        char buf[64];
        const int len =
            std::snprintf(buf, sizeof buf, "app: job pid=%d seq=%d done\n",
                          ::getpid(), seq);
        write_line(std::string_view(buf, static_cast<std::size_t>(len)));
        ::_exit(0);
    }
}

// The persistent "worker": the long-running child that stands in for a
// real service. It installs no handlers of its own -- an ordinary
// (non-PID-1) process, so SIGTERM's default action (terminate) applies the
// moment `serve` forwards the signal to it.
[[nodiscard]] std::expected<pid_t, std::error_code> spawn_worker() {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return std::unexpected(errno_ec());
    }
    if (pid == 0) {
        // Undo the parent's SIG_BLOCK: without this the child inherits
        // TERM/INT/CHLD as *blocked*, so a forwarded SIGTERM would just sit
        // pending forever instead of running its default (terminate)
        // action -- the worker would be unkillable. Same fix pmon (ch. 12)
        // applies before execvp; here there is no exec, so it happens
        // directly.
        sigset_t empty;
        sigemptyset(&empty);
        ::sigprocmask(SIG_SETMASK, &empty, nullptr);
        for (int tick = 1;; ++tick) {
            ::sleep(2);
            char buf[64];
            const int len = std::snprintf(
                buf, sizeof buf, "app: worker pid=%d tick=%d\n", ::getpid(), tick);
            write_line(std::string_view(buf, static_cast<std::size_t>(len)));
        }
    }
    return pid;
}

// Reap every exited child without blocking, reporting each one. Called from
// the ordinary poll(2) loop below (never from a signal handler), so
// std::println here is fine.
void reap_all() {
    for (;;) {
        int status = 0;
        const pid_t pid = ::waitpid(-1, &status, WNOHANG);
        if (pid <= 0) {
            break;
        }
        if (WIFSIGNALED(status)) {
            std::println("app: reaped pid={} signal={}", pid, WTERMSIG(status));
        } else {
            std::println("app: reaped pid={} status={}", pid, WEXITSTATUS(status));
        }
    }
}

int serve() {
    print_container_line();
    std::println("app: pid={} ppid={}", ::getpid(), ::getppid());

    // Block SIGTERM/SIGINT/SIGCHLD and receive them as signalfd(2) reads, so
    // no code ever runs in real signal-handler context (the pmon pattern
    // from chapter 12).
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGCHLD);
    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
        die("sigprocmask", errno_ec());
    }
    const int sfd = ::signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd < 0) {
        die("signalfd", errno_ec());
    }

    const auto worker = spawn_worker();
    if (!worker) {
        die("fork worker", worker.error());
    }
    const pid_t worker_pid = *worker;
    std::println("app: worker started pid={}", worker_pid);

    // A std::jthread drives the periodic "job" spawns: RAII-managed, and its
    // stop_token lets shutdown cancel it cooperatively instead of detaching
    // or leaking a thread.
    std::atomic<int> next_seq{0};
    std::jthread job_spawner([&](const std::stop_token& stok) {
        while (!stok.stop_requested()) {
            for (int i = 0; i < 10 && !stok.stop_requested(); ++i) {
                std::this_thread::sleep_for(100ms);
            }
            if (stok.stop_requested()) {
                break;
            }
            spawn_job(next_seq.fetch_add(1) + 1);
        }
    });

    for (;;) {
        pollfd pfd{.fd = sfd, .events = POLLIN, .revents = 0};
        const int nready = ::poll(&pfd, 1, -1);
        if (nready < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("poll", errno_ec());
        }

        signalfd_siginfo si{};
        const ssize_t nread = ::read(sfd, &si, sizeof si);
        if (nread != static_cast<ssize_t>(sizeof si)) {
            die("read signalfd", errno_ec());
        }

        if (si.ssi_signo == static_cast<uint32_t>(SIGCHLD)) {
            reap_all();
            continue;
        }

        // SIGTERM or SIGINT: this is the pid-1 duty naive skips. Stop
        // spawning new jobs, forward the signal to the worker, reap it, and
        // only then exit -- so nothing is left behind when the container
        // stops.
        job_spawner.request_stop();
        if (::kill(worker_pid, SIGTERM) == 0 || errno == ESRCH) {
            int status = 0;
            ::waitpid(worker_pid, &status, 0);
        }
        std::println("app: shutting down ({})",
                     si.ssi_signo == static_cast<uint32_t>(SIGTERM) ? "SIGTERM"
                                                                     : "SIGINT");
        return 0;
    }
}

// THE OTHER HALF OF THE TRAP: naive installs no signal handling whatsoever.
// As this container's PID 1, the kernel will not run SIGTERM's default
// action (terminate) for it -- there is no handler, so the signal is simply
// dropped. `podman stop` has no way to ask it to leave; only SIGKILL ends it.
int naive() {
    print_container_line();
    std::println("app: pid={} ppid={}", ::getpid(), ::getppid());
    for (int tick = 1;; ++tick) {
        std::this_thread::sleep_for(1s);
        std::println("app: naive heartbeat tick={}", tick);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // line-buffered even into a pipe

    if (argc != 2) {
        usage();
    }
    const std::string_view sub = argv[1];
    if (sub == "serve") {
        return serve();
    }
    if (sub == "naive") {
        return naive();
    }
    usage();
}
