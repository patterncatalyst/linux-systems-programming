// capstone -- the reference program for ch56, closing Part 14.
//
// ONE workload (workload.hpp), SIX models, TWO uniform instruments.
//
// Nothing here re-derives ch50-ch55. Each arm is the smallest correct way to
// run the shared workload in its model, because the novelty of this chapter is
// not any model -- it is that all of them are finally measured the same way:
//
//   instrument 1  distinct_tids, from gettid()          -- ch50's, ch49's terms
//   instrument 2  futex calls, from outside the process -- ch51's, via strace
//
// Two further axes (bytes per paused computation, and the shape cancellation
// arrives in) are ASSEMBLED in the `table` arm from numbers earlier chapters
// measured, carried as named constants with their provenance. They are not
// re-measured here and the chapter says so.
//
// Nothing is timed (ch39). Nothing binds a port. No third-party fetch: system
// Boost only. The seventh model, P2300 senders, lives in senders.cpp and is
// built only by the opt-in Conan sub-target.

#include <coroutine>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

#include <boost/asio.hpp>
#include <boost/fiber/all.hpp>
#include <boost/thread/future.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <boost/version.hpp>

#include "workload.hpp"

namespace asio = boost::asio;
using namespace lsp56;

namespace {

// ── 0. the control: one thread, no synchronization at all ────────────────

void arm_sequential() {
    note_tid_once();
    std::uint64_t acc = 0;
    for (int t = 0; t < kTasks; ++t) {
        for (int r = 0; r < kRounds; ++r) {
            acc += unit(t, r);
        }
    }
    report_arm("sequential", acc);
}

// ── 1. pthreads (ch50) ───────────────────────────────────────────────────

struct PthreadArg {
    int task;
    std::uint64_t* acc;
    pthread_mutex_t* mutex;
};

void* pthread_body(void* raw) {
    auto* a = static_cast<PthreadArg*>(raw);
    note_tid_once();
    for (int r = 0; r < kRounds; ++r) {
        pthread_mutex_lock(a->mutex);
        *a->acc += unit(a->task, r);
        pthread_mutex_unlock(a->mutex);
    }
    return nullptr;
}

void arm_pthreads() {
    std::uint64_t acc = 0;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    std::vector<pthread_t> threads(kTasks);
    std::vector<PthreadArg> args(kTasks);

    for (int t = 0; t < kTasks; ++t) {
        args[t] = PthreadArg{t, &acc, &mutex};
        pthread_create(&threads[t], nullptr, &pthread_body, &args[t]);
    }
    for (auto& thread : threads) {
        pthread_join(thread, nullptr);
    }
    report_arm("pthreads", acc);
}

// ── 2. the standard threading library (ch51) ─────────────────────────────

void arm_std_thread() {
    std::uint64_t acc = 0;
    std::mutex m;
    {
        std::vector<std::jthread> threads;
        threads.reserve(kTasks);
        for (int t = 0; t < kTasks; ++t) {
            threads.emplace_back([t, &acc, &m] {
                note_tid_once();
                for (int r = 0; r < kRounds; ++r) {
                    const std::lock_guard<std::mutex> lock(m);
                    acc += unit(t, r);
                }
            });
        }
    }  // jthread joins in its destructor -- ch51's whole point
    report_arm("std-thread", acc);
}

// ── 3. Boost.Thread (ch52) ───────────────────────────────────────────────
//
// Same workload, but joined the way ch52 established: futures composed with
// when_all rather than a loop of join() calls.

void arm_boost_thread() {
    std::uint64_t acc = 0;
    boost::mutex m;
    std::vector<boost::future<void>> futures;
    futures.reserve(kTasks);

    for (int t = 0; t < kTasks; ++t) {
        futures.push_back(boost::async(boost::launch::async, [t, &acc, &m] {
            note_tid_once();
            for (int r = 0; r < kRounds; ++r) {
                const boost::lock_guard<boost::mutex> lock(m);
                acc += unit(t, r);
            }
        }));
    }
    boost::when_all(futures.begin(), futures.end()).get();
    report_arm("boost-thread", acc);
}

// ── 4. C++20 coroutines (ch53) ───────────────────────────────────────────
//
// Eight tasks in flight on ONE thread. No mutex anywhere, and none needed:
// a coroutine runs until it suspends, so between two co_awaits it has the
// accumulator to itself. ch49's grid, bottom-left cell.

struct FoldTask {
    struct promise_type {
        FoldTask get_return_object() {
            return FoldTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };

    explicit FoldTask(std::coroutine_handle<promise_type> handle) : h(handle) {}
    FoldTask(FoldTask&& other) noexcept : h(other.h) { other.h = {}; }
    FoldTask(const FoldTask&) = delete;
    FoldTask& operator=(const FoldTask&) = delete;
    ~FoldTask() {
        if (h) {
            h.destroy();
        }
    }

    bool done() const { return h.done(); }
    void resume() { h.resume(); }

    std::coroutine_handle<promise_type> h;
};

FoldTask fold_coroutine(int task, std::uint64_t& acc) {
    for (int r = 0; r < kRounds; ++r) {
        acc += unit(task, r);
        co_await std::suspend_always{};
    }
}

void arm_coroutine() {
    note_tid_once();
    std::uint64_t acc = 0;
    std::vector<FoldTask> tasks;
    tasks.reserve(kTasks);
    for (int t = 0; t < kTasks; ++t) {
        tasks.push_back(fold_coroutine(t, acc));
    }

    // The scheduler, in full. ch27 had to hand-roll one of these over epoll;
    // here there is no I/O, so round-robin resumption is the whole runtime.
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (auto& task : tasks) {
            if (!task.done()) {
                task.resume();
                progressed = true;
            }
        }
    }
    report_arm("coroutine", acc);
}

// ── 5. Boost.Fiber (ch54) ────────────────────────────────────────────────
//
// Also eight in flight on one thread, and also no mutex -- but suspension is
// a stack switch rather than a state-machine transition, which is what ch54
// priced at 131072 bytes apiece.

void arm_fiber() {
    note_tid_once();
    std::uint64_t acc = 0;
    std::vector<boost::fibers::fiber> fibers;
    fibers.reserve(kTasks);
    for (int t = 0; t < kTasks; ++t) {
        fibers.emplace_back([t, &acc] {
            for (int r = 0; r < kRounds; ++r) {
                acc += unit(t, r);
                boost::this_fiber::yield();
            }
        });
    }
    for (auto& fiber : fibers) {
        fiber.join();
    }
    report_arm("fiber", acc);
}

// ── 6. Boost.Asio (ch55) ─────────────────────────────────────────────────
//
// Eight threads, one io_context, and the accumulator guarded by a STRAND
// rather than a mutex: one fold per handler, serialized without a lock.

void arm_asio() {
    std::uint64_t acc = 0;
    asio::io_context ioc;
    auto strand = asio::make_strand(ioc);

    for (int t = 0; t < kTasks; ++t) {
        for (int r = 0; r < kRounds; ++r) {
            asio::post(strand, [t, r, &acc] { acc += unit(t, r); });
        }
    }

    std::vector<std::thread> threads;
    threads.reserve(kTasks);
    for (int i = 0; i < kTasks; ++i) {
        threads.emplace_back([&ioc] {
            note_tid_once();
            ioc.run();
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    report_arm("asio", acc);
}

// ── the assembled tables ─────────────────────────────────────────────────
//
// These are NOT measured here. Each number was measured by the chapter named
// beside it, with the instrument named beside it, and is carried forward as a
// named constant so the table cites real prior work rather than restating it
// from memory. ch54 established this pattern.

constexpr std::size_t kCh50ThreadStackBytes = 8388608;    // pthread_getattr_np
constexpr std::size_t kCh54FiberStackBytes = 131072;      // stack_traits::default_size()
constexpr std::size_t kCh53CoroutineFrameBytes = 32;      // operator new in promise_type

void arm_table() {
    note_tid_once();
    std::printf("table: paused-computation cost, assembled from earlier chapters\n");
    std::printf("table: thread=%zu (ch50, pthread_getattr_np)\n", kCh50ThreadStackBytes);
    std::printf("table: fiber=%zu (ch54, stack_traits::default_size)\n", kCh54FiberStackBytes);
    std::printf("table: coroutine_frame=%zu (ch53, operator new in promise_type)\n",
                kCh53CoroutineFrameBytes);
    std::printf("table: fibers_per_thread_stack=%zu frames_per_fiber_stack=%zu\n",
                kCh50ThreadStackBytes / kCh54FiberStackBytes,
                kCh54FiberStackBytes / kCh53CoroutineFrameBytes);

    std::printf("table: cancellation shapes, four of them, all previously measured\n");
    std::printf("table: ch50 pthread_cancel=forced-unwind (swallowing it aborts the process)\n");
    std::printf("table: ch51 stop_token=flag (nothing is thrown; the target polls)\n");
    std::printf("table: ch52 thread::interrupt=exception (ordinary, at defined points)\n");
    std::printf("table: ch55 cancellation_signal=completion (operation_aborted, handler still runs)\n");
    report_arm("table", expected_total());
}

// ── versions, including the ch49 debt ────────────────────────────────────

void arm_versions() {
    note_tid_once();
    std::printf("versions: Boost %d.%d.%d\n", BOOST_VERSION / 100000, (BOOST_VERSION / 100) % 1000,
                BOOST_VERSION % 100);
#ifdef __cpp_lib_senders
    std::printf("versions: senders_macro=%ld stdlib_p2300=yes\n",
                static_cast<long>(__cpp_lib_senders));
#else
    std::printf("versions: senders_macro=undefined stdlib_p2300=no\n");
#endif
#ifdef __cpp_lib_execution
    std::printf("versions: execution_macro=%ld (201902 = the C++17 parallel algorithms header)\n",
                static_cast<long>(__cpp_lib_execution));
#endif
    std::printf("versions: the P2300 arm is built only by the opt-in Conan sub-target\n");
    report_arm("versions", expected_total());
}

void usage() {
    std::fprintf(stderr, "usage: capstone <versions|sequential|pthreads|std-thread|boost-thread|"
                         "coroutine|fiber|asio|table>\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        usage();
        return 2;
    }
    const std::string what = argv[1];

    if (what == "versions") {
        arm_versions();
    } else if (what == "sequential") {
        arm_sequential();
    } else if (what == "pthreads") {
        arm_pthreads();
    } else if (what == "std-thread") {
        arm_std_thread();
    } else if (what == "boost-thread") {
        arm_boost_thread();
    } else if (what == "coroutine") {
        arm_coroutine();
    } else if (what == "fiber") {
        arm_fiber();
    } else if (what == "asio") {
        arm_asio();
    } else if (what == "table") {
        arm_table();
    } else {
        usage();
        return 2;
    }
    return 0;
}
