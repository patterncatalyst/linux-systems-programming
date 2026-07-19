// app work --seconds N — a small, observable workload for the eBPF
// observation toolkit (chapter 30). It does four things in a loop, each one
// deliberate bait for a specific bcc-tools/bpftrace probe:
//
//   1. opens (create+write+close) a fixed file every iteration  -> opensnoop
//   2. fork/execs a short-lived child ("true") every 4th iter    -> execsnoop
//   3. calls the named hot function busy_hash() every iteration  -> funccount
//                                                                    / uprobe
//   4. sleeps most of the iteration, so the process is off-CPU    -> offcputime
//
// This file writes no kernel-side eBPF: it is the userspace *target* that
// examples/30-ebpf-observation-toolkit/observe.sh (running as root on the lab
// VM) points bcc-tools and bpftrace at.
//
// C++23 idioms: a std::jthread (with std::stop_token) runs the workload loop
// so main() gets automatic join-on-scope-exit; an RAII Fd wraps the bait
// file's descriptor; std::expected carries every fallible syscall; output
// goes through std::println.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <print>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

constexpr auto kIterInterval = 230ms;
constexpr int kExecEveryNIters = 4;
constexpr std::uint64_t kBusyRounds = 300'000;
const char* kBaitPath = "/tmp/lsp-ebpf-work-bait.txt";

// The named hot function every uprobe/funccount/bpftrace command in this
// chapter targets by name. extern "C" so the symbol isn't mangled, matching
// the Rust build; noinline so -O2 can't fold it into the caller and make the
// uprobe's attach point disappear.
extern "C" [[gnu::noinline]] std::uint64_t busy_hash(std::uint64_t x) noexcept {
    for (std::uint64_t i = 0; i < kBusyRounds; ++i) {
        x ^= x << 7;
        x ^= x >> 9;
    }
    return x;
}

// RAII wrapper: closes the fd no matter how the scope is left.
class Fd {
  public:
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

  private:
    int fd_;
};

// opensnoop bait: create/truncate, write a line, close. Every call is a fresh
// open(2) on the same path.
[[nodiscard]] std::expected<void, std::error_code> open_bait(long iter) {
    // 0666: the bait file may get created by a root-run observe.sh one time
    // and an unprivileged demo.sh run the next — world-writable avoids a
    // permission mismatch between those two ownership scenarios.
    Fd fd{::open(kBaitPath, O_CREAT | O_WRONLY | O_TRUNC, 0666)};
    if (!fd.valid()) {
        return std::unexpected(std::error_code{errno, std::system_category()});
    }
    const std::string line = "iter " + std::to_string(iter) + "\n";
    if (::write(fd.get(), line.data(), line.size()) < 0) {
        return std::unexpected(std::error_code{errno, std::system_category()});
    }
    return {};
}

// execsnoop bait: fork + execvp("true"), wait for it. Returns the child's
// exit status, or a syscall error.
[[nodiscard]] std::expected<int, std::error_code> spawn_true() {
    const pid_t child = ::fork();
    if (child < 0) {
        return std::unexpected(std::error_code{errno, std::system_category()});
    }
    if (child == 0) {
        char* argv[] = {const_cast<char*>("true"), nullptr};
        ::execvp("true", argv);
        _exit(127); // execvp only returns on failure
    }
    int wstatus = 0;
    if (::waitpid(child, &wstatus, 0) < 0) {
        return std::unexpected(std::error_code{errno, std::system_category()});
    }
    return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
}

struct Counters {
    std::atomic<long> iters{0};
    std::atomic<long> opens{0};
    std::atomic<long> execs{0};
    std::atomic<long> busy_calls{0};
    // The accumulated busy_hash() result. Printed in the summary line below —
    // that's what keeps the optimizer from proving the whole busy_hash call
    // chain's result is unused and deleting it outright (noinline only stops
    // inlining, not interprocedural dead-code elimination of a provably-pure,
    // provably-unused call chain).
    std::atomic<std::uint64_t> busy_hash_val{0};
};

void run_workload(std::stop_token stoken, int seconds, Counters& c) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    long iter = 0;
    std::uint64_t acc = 1;
    while (!stoken.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        if (auto r = open_bait(iter); r) {
            c.opens.fetch_add(1, std::memory_order_relaxed);
        } else {
            std::println(stderr, "work: open bait failed: {}", r.error().message());
        }

        if (iter % kExecEveryNIters == 0) {
            if (auto r = spawn_true(); r) {
                c.execs.fetch_add(1, std::memory_order_relaxed);
                std::println("work: exec i={} status={}", iter, *r);
            } else {
                std::println(stderr, "work: spawn failed: {}", r.error().message());
            }
        }

        acc = busy_hash(acc);
        c.busy_calls.fetch_add(1, std::memory_order_relaxed);

        std::this_thread::sleep_for(kIterInterval);
        ++iter;
    }
    c.iters.store(iter, std::memory_order_relaxed);
    c.busy_hash_val.store(acc, std::memory_order_relaxed);
}

int cmd_work(int seconds) {
    std::println("work: start seconds={} pid={} bait={}", seconds, ::getpid(), kBaitPath);
    Counters counters;
    // jthread: takes a stop_token as its first parameter (so it's ready for
    // cooperative cancellation, e.g. from a signal handler) and joins on
    // request/destruction like a plain thread — join explicitly here so the
    // summary line below always sees the final counters.
    std::jthread worker(run_workload, seconds, std::ref(counters));
    worker.join();
    std::println("work: done seconds={} iters={} opens={} execs={} busy_calls={} busy_hash={}",
                 seconds, counters.iters.load(), counters.opens.load(), counters.execs.load(),
                 counters.busy_calls.load(), counters.busy_hash_val.load());
    return 0;
}

void usage() {
    std::println(stderr, "usage: app work --seconds N");
}

} // namespace

int main(int argc, char** argv) {
    // glibc fully-buffers stdout by default once it's not a terminal (e.g.
    // redirected to observe.sh's log file), so progress lines would sit in a
    // libc buffer instead of reaching a live "tail -f"/"head" the way Go's
    // and Rust's stdout already do. Force line buffering so every std::println
    // shows up the moment it's printed, redirected or not.
    std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);

    if (argc < 2 || std::string_view(argv[1]) != "work") {
        usage();
        return 2;
    }
    int seconds = -1;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--seconds" && i + 1 < argc) {
            char* end = nullptr;
            const long v = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v <= 0) {
                std::println(stderr, "work: bad --seconds value: {}", argv[i]);
                return 2;
            }
            seconds = static_cast<int>(v);
        } else {
            usage();
            return 2;
        }
    }
    if (seconds <= 0) {
        usage();
        return 2;
    }
    return cmd_work(seconds);
}
