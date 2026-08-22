// asiodemo -- the reference program for ch55.
//
// ch09 built an epoll loop by hand. ch22 wrote the nonblocking/EAGAIN state
// machine. ch27 drove a hand-rolled coroutine reactor over epoll. All three
// own the readiness model, and NOTHING here repeats it.
//
// This program is about what a library adds on top of that reactor -- the
// concepts Asio has that a hand-written loop does not, each measured:
//
//   work-guard    why io_context::run() returns immediately, and the fix
//   strand        serialized handlers WITHOUT a mutex -- and the data race
//   nostrand      the same code without it, losing increments for real
//   strand-cost   strand vs mutex, counted in futex calls (ch51's method)
//   mutex-cost    the comparison arm
//   gather        4 buffers -> ONE write-family syscall
//   separate      the same 4 buffers -> four syscalls
//   topology-*    who actually runs the handlers, by gettid()
//   cancel        cancellation_signal vs ch51's std::stop_token
//
// Nothing here binds a port: the I/O demos use a AF_UNIX socketpair via
// local::connect_pair, so the example stays offline and unprivileged.
//
// Nothing here is timed (see ch39). Every observable is a handler count, a
// syscall count, a tid count, or a value that is either correct or not.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <boost/asio.hpp>
#include <boost/version.hpp>

namespace asio = boost::asio;

namespace {

// ch46-ch54's payload ("The quick brown."), carried forward unchanged.
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

void report(const char* what) {
    std::printf("asio report: case=%s digest=0x%016llx\n", what,
                static_cast<unsigned long long>(fnv1a(kPayload, sizeof kPayload)));
}

constexpr int kHandlerThreads = 8;
constexpr int kPosts = 2000;
constexpr int kCostPosts = 20000;

// ── work guard: why run() returns immediately ────────────────────────────
//
// The most common first surprise in Asio. io_context::run() returns when the
// context has no OUTSTANDING WORK -- not when you have finished with it. A
// freshly constructed context has none, so run() returns instantly and every
// async operation you were about to start never happens.
//
// executor_work_guard is a claim on the context that says "more work is
// coming, do not return yet". Releasing it lets run() finish.

void case_work_guard() {
    asio::io_context idle;
    const std::size_t ran_without = idle.run();

    asio::io_context guarded;
    auto guard = asio::make_work_guard(guarded);
    int posted = 0;
    asio::post(guarded, [&posted, &guard] {
        ++posted;
        guard.reset();  // release the claim; run() may now return
    });
    const std::size_t ran_with = guarded.run();

    std::printf("work-guard: without_work_handlers=%zu with_guard_handlers=%zu posted=%d\n",
                ran_without, ran_with, posted);
    std::printf("work-guard: returned_immediately=%s guard_kept_it_alive=%s\n",
                ran_without == 0 ? "yes" : "no", ran_with > 0 ? "yes" : "no");
    std::printf("work-guard: run() returns on no OUTSTANDING WORK, not on completion\n");
    report("work-guard");
}

// ── the strand ───────────────────────────────────────────────────────────
//
// A strand is an executor that guarantees the handlers posted through it never
// run concurrently -- and never overlap even when eight threads are pulling
// from the same io_context.
//
// The counter below is a plain `long`, deliberately NOT atomic and NOT under
// any lock. Through a strand that is correct by construction. Posted straight
// to the io_context it is a data race, and the demo reports what that costs.

struct Observed {
    std::atomic<int> inflight{0};
    std::atomic<int> max_inflight{0};
    std::atomic<int> overlaps{0};
    long unguarded = 0;  // intentionally unsynchronized -- that is the point
};

void bump_max(Observed& o, int now) {
    int seen = o.max_inflight.load(std::memory_order_relaxed);
    while (now > seen && !o.max_inflight.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
        // compare_exchange_weak refreshes `seen`; loop until we win or lose
    }
}

void run_on_threads(asio::io_context& ioc) {
    std::vector<std::thread> threads;
    threads.reserve(kHandlerThreads);
    for (int i = 0; i < kHandlerThreads; ++i) {
        threads.emplace_back([&ioc] { ioc.run(); });
    }
    for (auto& t : threads) {
        t.join();
    }
}

// The handler body, shared by both variants so the ONLY difference between
// them is which executor it was posted to.
auto make_handler(Observed& o) {
    return [&o] {
        const int now = o.inflight.fetch_add(1, std::memory_order_relaxed) + 1;
        bump_max(o, now);
        if (now > 1) {
            o.overlaps.fetch_add(1, std::memory_order_relaxed);
        }
        // Widen the window so overlap is observable rather than theoretical.
        for (volatile int k = 0; k < 200; ++k) {
        }
        ++o.unguarded;
        o.inflight.fetch_sub(1, std::memory_order_relaxed);
    };
}

void case_strand() {
    Observed o;
    asio::io_context ioc;
    auto strand = asio::make_strand(ioc);
    for (int i = 0; i < kPosts; ++i) {
        asio::post(strand, make_handler(o));
    }
    run_on_threads(ioc);

    std::printf("strand: posts=%d counter=%ld expected=%d correct=%s\n", kPosts, o.unguarded, kPosts,
                o.unguarded == kPosts ? "yes" : "no");
    std::printf("strand: max_inflight=%d overlaps=%d serialized=%s\n", o.max_inflight.load(),
                o.overlaps.load(), o.overlaps.load() == 0 ? "yes" : "no");
    std::printf("strand: %d threads were running handlers, and no two ran at once\n",
                kHandlerThreads);
    report("strand");
}

void case_nostrand() {
    Observed o;
    asio::io_context ioc;
    for (int i = 0; i < kPosts; ++i) {
        asio::post(ioc, make_handler(o));  // straight to the context, no strand
    }
    run_on_threads(ioc);

    // The counter is reported but NEVER asserted on: an unsynchronized
    // increment is a data race, which is undefined behavior, and the number
    // of lost updates is not reproducible. What IS a real observation is that
    // handlers overlapped -- that is measured with atomics and is well defined.
    std::printf("nostrand: posts=%d counter=%ld expected=%d lost=%ld\n", kPosts, o.unguarded, kPosts,
                static_cast<long>(kPosts) - o.unguarded);
    std::printf("nostrand: max_inflight=%d overlaps=%d serialized=%s\n", o.max_inflight.load(),
                o.overlaps.load(), o.overlaps.load() == 0 ? "yes" : "no");
    std::printf("nostrand: same handler, same threads -- only the executor changed\n");
    report("nostrand");
}

// ── a strand is not a mutex ──────────────────────────────────────────────
//
// Both of these produce the correct count. The difference is what they do to
// a thread that loses the race for the shared resource:
//
//   mutex   BLOCKS the thread -- and ch51 measured what blocking costs:
//           futex(FUTEX_WAIT) into the kernel, then FUTEX_WAKE to get out
//   strand  DEFERS the handler and returns the thread to the pool, where it
//           picks up other work. Nothing blocks, so nothing sleeps.
//
// Count them from outside with ch51's method:
//   strace -f -c -e trace=futex ./asiodemo strand-cost

void case_strand_cost() {
    long counter = 0;
    asio::io_context ioc;
    auto strand = asio::make_strand(ioc);
    for (int i = 0; i < kCostPosts; ++i) {
        asio::post(strand, [&counter] { ++counter; });
    }
    run_on_threads(ioc);
    std::printf("strand-cost: posts=%d counter=%ld correct=%s\n", kCostPosts, counter,
                counter == kCostPosts ? "yes" : "no");
    report("strand-cost");
}

void case_mutex_cost() {
    long counter = 0;
    std::mutex m;
    asio::io_context ioc;
    for (int i = 0; i < kCostPosts; ++i) {
        asio::post(ioc, [&counter, &m] {
            const std::lock_guard<std::mutex> lock(m);
            ++counter;
        });
    }
    run_on_threads(ioc);
    std::printf("mutex-cost: posts=%d counter=%ld correct=%s\n", kCostPosts, counter,
                counter == kCostPosts ? "yes" : "no");
    report("mutex-cost");
}

// ── scatter-gather ───────────────────────────────────────────────────────
//
// Asio's buffer sequences are not a convenience wrapper around a loop. A
// sequence maps onto the kernel's iovec form, so four buffers leave in ONE
// syscall -- which is the same readv/writev machinery Part 5 covered, reached
// without assembling an iovec array by hand.
//
// A socketpair rather than a real socket: nothing binds, nothing listens,
// the example stays offline.

struct Pair {
    asio::io_context ioc;
    asio::local::stream_protocol::socket a{ioc};
    asio::local::stream_protocol::socket b{ioc};
    Pair() { asio::local::connect_pair(a, b); }
};

const std::string kStatus = "HTTP/1.1 200 OK\r\n";
const std::string kType = "Content-Type: text/plain\r\n";
const std::string kBlank = "\r\n";
const std::string kBody = "hello world";

std::size_t drain(asio::local::stream_protocol::socket& s) {
    std::vector<char> in(4096);
    boost::system::error_code ec;
    return asio::read(s, asio::buffer(in), ec);
}

void case_gather() {
    Pair p;
    const std::array<asio::const_buffer, 4> bufs{asio::buffer(kStatus), asio::buffer(kType),
                                                 asio::buffer(kBlank), asio::buffer(kBody)};
    const std::size_t wrote = asio::write(p.a, bufs);
    p.a.close();
    const std::size_t got = drain(p.b);

    std::printf("gather: buffers=4 wrote=%zu peer_read=%zu intact=%s\n", wrote, got,
                wrote == got ? "yes" : "no");
    std::printf("gather: one buffer SEQUENCE -> one write-family syscall carrying 4 iovecs\n");
    report("gather");
}

void case_separate() {
    Pair p;
    std::size_t wrote = 0;
    for (const std::string* s : {&kStatus, &kType, &kBlank, &kBody}) {
        wrote += asio::write(p.a, asio::buffer(*s));  // four separate calls
    }
    p.a.close();
    const std::size_t got = drain(p.b);

    std::printf("separate: buffers=4 wrote=%zu peer_read=%zu intact=%s\n", wrote, got,
                wrote == got ? "yes" : "no");
    std::printf("separate: identical bytes on the wire, four times the syscalls\n");
    report("separate");
}

// ── topologies: who runs the handlers ────────────────────────────────────
//
// ch49 asked how many CPUs a workload occupied; ch54 asked how many threads
// fibers migrated across. Same question, same instrument (gettid()), applied
// to an I/O runtime -- and in Asio the answer is entirely your choice of how
// you call run().

std::mutex g_tid_mutex;
std::set<long> g_tids;

void note_tid() {
    const std::lock_guard<std::mutex> lock(g_tid_mutex);
    g_tids.insert(static_cast<long>(gettid()));
}

constexpr int kTopoPosts = 400;
constexpr int kTopoThreads = 4;

void case_topology(const std::string& shape) {
    g_tids.clear();

    if (shape == "one") {
        // One thread, one context. Every handler runs on the caller.
        asio::io_context ioc;
        for (int i = 0; i < kTopoPosts; ++i) {
            asio::post(ioc, &note_tid);
        }
        ioc.run();
    } else if (shape == "pool") {
        // N threads all calling run() on ONE context: a shared queue.
        asio::io_context ioc;
        for (int i = 0; i < kTopoPosts; ++i) {
            asio::post(ioc, &note_tid);
        }
        run_on_threads(ioc);
    } else if (shape == "percore") {
        // One context per thread: no sharing, no cross-thread handoff.
        std::vector<std::thread> threads;
        threads.reserve(kTopoThreads);
        for (int t = 0; t < kTopoThreads; ++t) {
            threads.emplace_back([] {
                asio::io_context ioc;
                for (int i = 0; i < kTopoPosts / kTopoThreads; ++i) {
                    asio::post(ioc, &note_tid);
                }
                ioc.run();
            });
        }
        for (auto& t : threads) {
            t.join();
        }
    } else if (shape == "threadpool") {
        // thread_pool owns its threads: no run() call anywhere.
        asio::thread_pool pool(kTopoThreads);
        for (int i = 0; i < kTopoPosts; ++i) {
            asio::post(pool, &note_tid);
        }
        pool.join();
    }

    std::printf("topology-%s: handlers=%d distinct_tids=%zu\n", shape.c_str(), kTopoPosts,
                g_tids.size());
    report(("topology-" + shape).c_str());
}

// ── cancellation ─────────────────────────────────────────────────────────
//
// ch51 measured std::stop_token: a flag the target polls, nothing thrown.
// ch52 measured boost::thread::interrupt(): an ordinary exception at defined
// points. Asio's model is a third shape again -- a cancellation_signal whose
// slot the OPERATION subscribes to, so cancellation is delivered as a
// completion with an error code rather than as an exception or a flag.
//
// The operation still completes. It completes with operation_aborted, which
// means the handler runs exactly once either way and the ownership rules
// never change.

void case_cancel() {
    asio::io_context ioc;
    asio::cancellation_signal sig;

    boost::system::error_code seen;
    bool handler_ran = false;

    asio::steady_timer timer(ioc, std::chrono::hours(1));
    timer.async_wait(asio::bind_cancellation_slot(sig.slot(),
                                                 [&](const boost::system::error_code& ec) {
                                                     handler_ran = true;
                                                     seen = ec;
                                                 }));

    // Cancel before running: the operation is already queued.
    sig.emit(asio::cancellation_type::all);
    const std::size_t ran = ioc.run();

    std::printf("cancel: handlers_run=%zu handler_ran=%s error=%s aborted=%s\n", ran,
                handler_ran ? "yes" : "no", seen.message().c_str(),
                seen == asio::error::operation_aborted ? "yes" : "no");
    std::printf("cancel: a one-hour timer completed immediately -- cancellation arrives as a "
                "COMPLETION with an error code, not an exception (ch52) and not a flag (ch51)\n");
    report("cancel");
}

void case_versions() {
    std::printf("versions: Boost %d.%d.%d asio_header_only=yes\n", BOOST_VERSION / 100000,
                (BOOST_VERSION / 100) % 1000, BOOST_VERSION % 100);
    report("versions");
}

void usage() {
    std::fprintf(stderr, "usage: asiodemo <versions|work-guard|strand|nostrand|strand-cost|"
                         "mutex-cost|gather|separate|cancel|topology-one|topology-pool|"
                         "topology-percore|topology-threadpool>\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        usage();
        return 2;
    }
    const std::string what = argv[1];

    if (what == "versions") {
        case_versions();
    } else if (what == "work-guard") {
        case_work_guard();
    } else if (what == "strand") {
        case_strand();
    } else if (what == "nostrand") {
        case_nostrand();
    } else if (what == "strand-cost") {
        case_strand_cost();
    } else if (what == "mutex-cost") {
        case_mutex_cost();
    } else if (what == "gather") {
        case_gather();
    } else if (what == "separate") {
        case_separate();
    } else if (what == "cancel") {
        case_cancel();
    } else if (what.rfind("topology-", 0) == 0) {
        case_topology(what.substr(9));
    } else {
        usage();
        return 2;
    }
    return 0;
}
