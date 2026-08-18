// stdthread -- the reference program for ch51.
//
// ch50 showed what a std::thread IS on Linux (a pthread). This program asks
// what the standard library's SYNCHRONIZATION primitives are, and the answer
// is the same one twice:
//
//   when they block  -> futex(2), the syscall ch25 built by hand
//   when they do not -> nothing at all, not one syscall
//
// The second half is the one people get wrong. An uncontended std::mutex is
// an atomic compare-exchange and no kernel involvement whatsoever; the cost
// people attribute to "locking" is really the cost of CONTENTION.
//
// Every subcommand below is designed to be counted from outside with
//
//   strace -f -c -e trace=futex ./stdthread <subcommand>
//
// rather than to time itself. Nothing here reports a duration (see ch39); the
// observable is a syscall count, which is reproducible in sign across hosts
// even where its magnitude is not.
//
// `deadlock-naive` hangs on purpose. It is its own subcommand for the same
// reason ch50's cancel-swallow was: a subcommand that never returns must
// never be able to take the others with it. Run it under timeout(1).

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <latch>
#include <mutex>
#include <semaphore>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// ch46-ch50's payload ("The quick brown."), carried forward unchanged.
constexpr std::uint8_t kPayload[16] = {0x54, 0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63,
                                       0x6b, 0x20, 0x62, 0x72, 0x6f, 0x77, 0x6e, 0x2e};

constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x00000100000001b3ULL;

std::uint64_t fnv1a(const std::uint8_t* data, std::size_t len) {
    std::uint64_t h = kFnvOffsetBasis;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= kFnvPrime;  // uint64_t wraps by definition -- no UB to sanitize
    }
    return h;
}

void report(const char* what, long acc) {
    std::printf("stdthread report: case=%s acc=%ld digest=0x%016llx\n", what, acc,
                static_cast<unsigned long long>(fnv1a(kPayload, sizeof kPayload)));
}

// The three cost cases below all perform EXACTLY the same amount of work --
// kTotalOps increments -- so the only variable between them is how many
// threads are contending for the lock. That is what makes the futex counts
// comparable: same operations, same result, different kernel involvement.
constexpr long kTotalOps = 200000;
constexpr int kContendedThreads = 8;

// ── baseline: no mutex, no threads ───────────────────────────────────────
//
// The control. Whatever syscalls this run makes are the floor: process
// startup, the dynamic loader, and nothing else. The uncontended case is
// measured against THIS, not against zero, because a program that has linked
// libstdc++ has already made a handful of syscalls before main() runs.

void case_baseline() {
    long n = 0;
    for (long i = 0; i < kTotalOps; ++i) {
        ++n;
    }
    report("baseline", n);
}

// ── uncontended: one thread, one mutex, 200k lock/unlock pairs ───────────
//
// std::mutex is a pthread_mutex_t is a futex word. Locking it when it is
// free is a single atomic compare-exchange in user space; glibc only issues
// futex(FUTEX_WAIT) when the exchange fails, which it never does here
// because nobody else is asking.

void case_uncontended() {
    std::mutex m;
    long n = 0;
    for (long i = 0; i < kTotalOps; ++i) {
        std::lock_guard<std::mutex> g(m);
        ++n;
    }
    report("uncontended", n);
}

// ── contended: eight threads, one mutex, the same 200k pairs ─────────────
//
// Identical total work, split across threads that actually collide. Now the
// compare-exchange fails often, and every failure is a trip into the kernel
// to sleep on the futex word plus another to wake a waiter.

void case_contended() {
    std::mutex m;
    long n = 0;
    std::vector<std::jthread> threads;
    threads.reserve(kContendedThreads);
    for (int t = 0; t < kContendedThreads; ++t) {
        threads.emplace_back([&m, &n] {
            for (long i = 0; i < kTotalOps / kContendedThreads; ++i) {
                std::lock_guard<std::mutex> g(m);
                ++n;
            }
        });
    }
    threads.clear();  // jthread joins on destruction
    report("contended", n);
}

// ── the blocking primitives ──────────────────────────────────────────────
//
// Each of these makes a thread WAIT for something another thread will do.
// There is no way to do that in user space alone -- a thread that must sleep
// until further notice has to tell the kernel. So each one reaches futex(2),
// and the point of these cases is that they are all the same mechanism
// wearing different type names.
//
// Each spawns a waiter, lets it reach the blocking call, then satisfies it.
// The main thread's brief sleep is scaffolding to make the waiter reach its
// wait before the signal arrives -- it is not being measured, and no
// assertion anywhere depends on its length.

void settle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void case_condvar() {
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    long n = 0;

    std::jthread waiter([&] {
        std::unique_lock<std::mutex> lock(m);
        // The predicate form, always: a bare wait() may return spuriously,
        // and this overload re-checks the condition on every wakeup.
        cv.wait(lock, [&ready] { return ready; });
        n = 1;
    });

    settle();
    {
        std::lock_guard<std::mutex> g(m);
        ready = true;
    }
    cv.notify_one();
    waiter.join();
    report("condvar", n);
}

void case_latch() {
    // A latch is single-use: count down to zero and it stays there.
    std::latch gate{3};
    std::atomic<long> arrived{0};

    std::vector<std::jthread> waiters;
    for (int i = 0; i < 2; ++i) {
        waiters.emplace_back([&] {
            arrived.fetch_add(1, std::memory_order_relaxed);
            gate.arrive_and_wait();
        });
    }
    settle();
    gate.count_down();
    waiters.clear();
    report("latch", arrived.load());
}

void case_barrier() {
    // A barrier is reusable: it resets after each phase, and runs an optional
    // completion function on the thread that trips it.
    std::atomic<long> phases{0};
    std::barrier sync{2, [&phases]() noexcept { phases.fetch_add(1, std::memory_order_relaxed); }};

    std::jthread a([&] {
        sync.arrive_and_wait();
        sync.arrive_and_wait();
    });
    settle();
    sync.arrive_and_wait();
    sync.arrive_and_wait();
    a.join();
    report("barrier", phases.load());
}

void case_semaphore() {
    std::counting_semaphore<4> sem{0};
    long n = 0;
    std::jthread waiter([&] {
        sem.acquire();
        n = 1;
    });
    settle();
    sem.release();
    waiter.join();
    report("semaphore", n);
}

void case_atomic_wait() {
    // C++20 gave std::atomic itself wait/notify. On Linux this is the futex
    // with the type-system noise removed: wait(old) sleeps while the value is
    // still `old`, exactly the FUTEX_WAIT contract from ch25.
    std::atomic<int> flag{0};
    long n = 0;
    std::jthread waiter([&] {
        flag.wait(0);  // block while flag == 0
        n = flag.load();
    });
    settle();
    flag.store(7);
    flag.notify_one();
    waiter.join();
    report("atomic-wait", n);
}

// ── stop_token: the ch50 payoff ──────────────────────────────────────────
//
// ch50 showed that pthread_cancel unwinds through an exception a catch-all
// can fatally swallow. This is the alternative C++20 chose: request_stop()
// sets a flag, the target polls it where IT decides is safe, and nothing is
// thrown, unwound, or catchable. The thread stops because it agreed to.

void case_stoptoken() {
    std::atomic<long> loops{0};
    std::jthread worker([&loops](std::stop_token stop) {
        while (!stop.stop_requested()) {
            loops.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    settle();
    worker.request_stop();
    worker.join();
    // The worker chose its own stopping point and returned normally. There is
    // no PTHREAD_CANCELED here, no forced unwind, and no way for a catch (...)
    // to turn this into an abort.
    std::printf("stoptoken: stop_requested honored, worker returned normally\n");
    report("stoptoken", loops.load() > 0 ? 1 : 0);
}

// ── deadlock, both directions ────────────────────────────────────────────
//
// Two mutexes, two threads, opposing acquisition orders: the textbook
// deadlock. The textbook is right, and the standard library ships the fix.
//
// std::scoped_lock takes all its mutexes at once using a deadlock-avoidance
// algorithm (try, back off, retry) rather than locking them in argument
// order. So scoped_lock(a,b) on one thread and scoped_lock(b,a) on the other
// is SAFE -- the argument order does not matter, which is exactly the
// property lock_guard cannot give you.

constexpr long kDeadlockIters = 50000;

void case_deadlock_safe() {
    std::mutex a;
    std::mutex b;
    std::atomic<long> done{0};

    std::jthread t1([&] {
        for (long i = 0; i < kDeadlockIters; ++i) {
            std::scoped_lock lock(a, b);
            done.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::jthread t2([&] {
        for (long i = 0; i < kDeadlockIters; ++i) {
            // The OPPOSITE order, deliberately. scoped_lock does not care.
            std::scoped_lock lock(b, a);
            done.fetch_add(1, std::memory_order_relaxed);
        }
    });
    t1.join();
    t2.join();
    std::printf("deadlock-safe: completed %ld acquisitions in opposing orders\n", done.load());
    report("deadlock-safe", done.load());
}

[[noreturn]] void case_deadlock_naive() {
    // This never returns. Run it under timeout(1), and while it hangs, read
    // /proc/<pid>/task/*/wchan to see every thread parked in futex_do_wait.
    static std::mutex a;
    static std::mutex b;

    std::printf("deadlock-naive: two lock_guards in opposing orders; this will hang\n");
    std::fflush(stdout);

    std::thread t1([] {
        for (;;) {
            std::lock_guard<std::mutex> x(a);
            std::this_thread::sleep_for(std::chrono::microseconds(1));
            std::lock_guard<std::mutex> y(b);
        }
    });
    std::thread t2([] {
        for (;;) {
            std::lock_guard<std::mutex> x(b);
            std::this_thread::sleep_for(std::chrono::microseconds(1));
            std::lock_guard<std::mutex> y(a);
        }
    });
    t1.join();
    t2.join();
    std::abort();  // unreachable: the joins never return
}

void usage() {
    std::fprintf(stderr,
                 "usage: stdthread <baseline|uncontended|contended|condvar|latch|barrier|"
                 "semaphore|atomic-wait|stoptoken|deadlock-safe|deadlock-naive>\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        usage();
        return 2;
    }
    const std::string what = argv[1];

    if (what == "baseline") {
        case_baseline();
    } else if (what == "uncontended") {
        case_uncontended();
    } else if (what == "contended") {
        case_contended();
    } else if (what == "condvar") {
        case_condvar();
    } else if (what == "latch") {
        case_latch();
    } else if (what == "barrier") {
        case_barrier();
    } else if (what == "semaphore") {
        case_semaphore();
    } else if (what == "atomic-wait") {
        case_atomic_wait();
    } else if (what == "stoptoken") {
        case_stoptoken();
    } else if (what == "deadlock-safe") {
        case_deadlock_safe();
    } else if (what == "deadlock-naive") {
        case_deadlock_naive();
    } else {
        usage();
        return 2;
    }
    return 0;
}
