// pmon.cpp — the book's recurring process supervisor (ch11-14, 18-19, 32,
// 34), grown here into the capstone's fleet init: it drops the capability
// bounding set once (caps.cpp), then fork/execs (self-re-exec via
// /proc/self/exe, the same technique ch34's container entrypoint uses)
// chatterd, sysagent, and fwatch as children, restarting any of them that
// exits unexpectedly, forwarding SIGTERM/SIGINT to all three on its own
// shutdown, and printing a health line every tick so an operator (or
// verify.lua, over ssh) can poll fleet state without touching /proc.
//
// C++23: a sigwait(2) thread (blocking SIGTERM/SIGINT before any child
// thread starts, exactly ch40's rationale for blocking early) replaces Go's
// signal.Notify channel; a std::condition_variable stands in for Go's
// select-on-done-or-ticker; each forked child unblocks its own inherited
// signal mask before execve, the fix ch34's container entrypoint documents
// for the same reason (a mask blocked in the parent is otherwise inherited
// straight through fork+exec, and would leave the child's own SIGTERM
// handler unable to ever run).
#include "pmon.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <print>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "caps.hpp"

namespace pmon {

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string self_exe_path() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) {
        return {};
    }
    buf[n] = '\0';
    return std::string(buf);
}

struct ChildSpec {
    std::string name;
    std::vector<std::string> args;
};

enum class State { Starting, Up, Restarting, Down };

[[nodiscard]] std::string_view state_name(State s) {
    switch (s) {
    case State::Starting: return "starting";
    case State::Up: return "up";
    case State::Restarting: return "restarting";
    case State::Down: return "down";
    }
    return "?";
}

struct ChildState {
    std::atomic<State> state{State::Starting};
    std::atomic<long> restarts{0};
    std::atomic<pid_t> pid{0};
};

// run_child_supervisor: fork+exec `self_exe args...` in a loop, restarting on
// unexpected exit — the per-service goroutine from go/pmon.go, one std thread
// per service here too.
void run_child_supervisor(const std::string& self_exe, const ChildSpec& spec, ChildState& cs,
                           std::atomic<bool>& shutting_down) {
    for (;;) {
        const pid_t pid = ::fork();
        if (pid < 0) {
            std::println(stderr, "pmon: start service={}: {}", spec.name, std::strerror(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        if (pid == 0) {
            // Unblock what pmon::run blocked in the parent (see pmon::run):
            // a forwarded SIGTERM must reach this child's own handler, not
            // sit blocked forever.
            sigset_t empty;
            sigemptyset(&empty);
            ::pthread_sigmask(SIG_SETMASK, &empty, nullptr);

            std::vector<char*> argv;
            argv.reserve(spec.args.size() + 2);
            argv.push_back(const_cast<char*>(self_exe.c_str()));
            for (const auto& a : spec.args) {
                argv.push_back(const_cast<char*>(a.c_str()));
            }
            argv.push_back(nullptr);
            ::execv(self_exe.c_str(), argv.data());
            ::_exit(127); // execv failed
        }

        cs.pid.store(pid, std::memory_order_relaxed);
        cs.state.store(State::Up, std::memory_order_relaxed);
        std::println("pmon: started service={} pid={}", spec.name, pid);
        std::fflush(stdout);

        int status = 0;
        ::waitpid(pid, &status, 0);

        if (shutting_down.load(std::memory_order_relaxed)) {
            cs.state.store(State::Down, std::memory_order_relaxed);
            return;
        }

        const long n = cs.restarts.fetch_add(1, std::memory_order_relaxed) + 1;
        cs.state.store(State::Restarting, std::memory_order_relaxed);
        std::string reason;
        if (WIFEXITED(status)) {
            reason = "exit status " + std::to_string(WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            reason = std::string("signal: ") + ::strsignal(WTERMSIG(status));
        } else {
            reason = "unknown";
        }
        std::println("pmon: restart service={} attempt={} reason={}", spec.name, n, reason);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

void print_health(const std::vector<std::string>& order,
                   const std::unordered_map<std::string, ChildState*>& states) {
    std::string line = "pmon: health ";
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (i) line += " ";
        line += order[i];
        line += "=";
        line += state_name(states.at(order[i])->state.load(std::memory_order_relaxed));
    }
    line += " restarts=";
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (i) line += ",";
        line += order[i];
        line += ":";
        line += std::to_string(states.at(order[i])->restarts.load(std::memory_order_relaxed));
    }
    std::println("{}", line);
    std::fflush(stdout);
}

} // namespace

int run(const std::string& node, const std::string& sandbox_dir, const std::string& peer,
        const std::string& peer_node, int chatterd_port, int health_interval_ms) {
    std::error_code ec;
    fs::create_directories(sandbox_dir, ec);
    if (ec) {
        std::println(stderr, "pmon: mkdir {}: {}", sandbox_dir, ec.message());
        return 1;
    }

    caps::drop_bounding_set();

    const std::string self_exe = self_exe_path();
    if (self_exe.empty()) {
        std::println(stderr, "pmon: readlink /proc/self/exe: {}", std::strerror(errno));
        return 1;
    }

    std::vector<std::string> chatterd_args{"chatterd", "serve",           "--host", "0.0.0.0",
                                            "--port",   std::to_string(chatterd_port), "--node", node};
    if (!peer.empty()) {
        chatterd_args.push_back("--peer");
        chatterd_args.push_back(peer);
        chatterd_args.push_back("--peer-node");
        chatterd_args.push_back(peer_node);
    }

    const std::vector<ChildSpec> specs{
        {"chatterd", chatterd_args},
        {"sysagent", {"sysagent", "--node", node, "--interval-ms", "2000"}},
        {"fwatch", {"fwatch", "watch", sandbox_dir, "--sandbox"}},
    };

    std::unordered_map<std::string, ChildState> state_storage;
    state_storage.reserve(specs.size());
    for (const auto& s : specs) {
        state_storage[s.name];
    }
    std::unordered_map<std::string, ChildState*> states;
    for (const auto& s : specs) {
        states[s.name] = &state_storage[s.name];
    }

    std::atomic<bool> shutting_down{false};

    // Block SIGTERM/SIGINT in THIS thread before spawning anything: the
    // dedicated sigwait thread below is the only one that ever consumes
    // them; every forked child unblocks its own inherited copy of the mask
    // before execve (see run_child_supervisor).
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);

    std::thread signal_thread([&] {
        int sig = 0;
        if (::sigwait(&mask, &sig) == 0) {
            shutting_down.store(true, std::memory_order_relaxed);
            for (auto& [name, cs] : states) {
                (void)name;
                const pid_t pid = cs->pid.load(std::memory_order_relaxed);
                if (pid > 0) {
                    ::kill(pid, SIGTERM);
                }
            }
        }
    });

    const std::vector<std::string> order{"chatterd", "sysagent", "fwatch"};
    std::vector<std::thread> workers;
    std::atomic<int> active{static_cast<int>(specs.size())};
    std::mutex done_mu;
    std::condition_variable done_cv;

    for (const auto& spec : specs) {
        workers.emplace_back([&, spec] {
            run_child_supervisor(self_exe, spec, *states[spec.name], shutting_down);
            if (active.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> lock(done_mu);
                done_cv.notify_one();
            }
        });
    }

    {
        std::unique_lock<std::mutex> lock(done_mu);
        const auto interval = std::chrono::milliseconds(health_interval_ms);
        while (active.load(std::memory_order_acquire) > 0) {
            const bool done =
                done_cv.wait_for(lock, interval, [&] { return active.load(std::memory_order_acquire) == 0; });
            if (done) {
                break;
            }
            print_health(order, states);
        }
    }

    std::println("pmon: shutdown");
    std::fflush(stdout);

    for (auto& w : workers) {
        w.join();
    }
    signal_thread.detach(); // sigwait already returned by the time we get here

    return 0;
}

} // namespace pmon
