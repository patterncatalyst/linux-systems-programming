// boostthread -- the reference program for ch52.
//
// Boost.Thread is where std::thread came from: C++11 standardized it almost
// wholesale. So the interesting question in 2026 is not "how do I use
// Boost.Thread" but "what is still in it that the standard did not take?"
//
// This program answers that with four pillars, each a thing std:: cannot do
// on this toolchain, and each verified rather than asserted:
//
//   continuations  boost::future::then() and when_all -- std::future has
//                  neither, provable at compile time
//   upgrade        boost::upgrade_lock -> upgrade_to_unique_lock, promoting a
//                  reader to a writer without releasing. std::shared_mutex
//                  has no upgrade path at all; the name std::upgrade_lock
//                  does not exist.
//   interrupt      boost::thread::interrupt() -- the THIRD cancellation model
//                  in this book, sitting between ch50's pthread_cancel and
//                  ch51's stop_token
//   attributes     boost::thread::attributes::set_stack_size -- ch50's
//                  pthread_attr_setstacksize, portably
//
// BOOST_THREAD_VERSION and the continuation macros are set in CMakeLists.txt
// rather than here, deliberately: they must be defined before any Boost
// header is seen, and a define in this file would be silently defeated by an
// include that arrived first.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <future>
#include <string>
#include <thread>

#include <pthread.h>
#include <unistd.h>

#include <boost/thread.hpp>
#include <boost/thread/future.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <boost/version.hpp>

namespace {

// ch46-ch51's payload ("The quick brown."), carried forward unchanged.
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

void report(const char* pillar) {
    std::printf("boostthread report: pillar=%s digest=0x%016llx\n", pillar,
                static_cast<unsigned long long>(fnv1a(kPayload, sizeof kPayload)));
}

// ── the compile-time contrast ────────────────────────────────────────────
//
// Whether a type has .then() is decidable at compile time, so this chapter
// does not have to *claim* that std::future lacks continuations -- it can
// compile a concept against both types and print the two answers. A reader
// who doubts it can change the concept and watch the output change.

template <class F>
concept HasThen = requires(F f) { f.then([](F) {}); };

void pillar_versions() {
    std::printf("versions: Boost %d.%d.%d BOOST_THREAD_VERSION=%d\n", BOOST_VERSION / 100000,
                (BOOST_VERSION / 100) % 1000, BOOST_VERSION % 100, BOOST_THREAD_VERSION);
    std::printf("versions: std::future_has_then=%s boost::future_has_then=%s\n",
                HasThen<std::future<int>> ? "yes" : "no",
                HasThen<boost::future<int>> ? "yes" : "no");
    report("versions");
}

// ── pillar 1: continuations ──────────────────────────────────────────────
//
// std::future can be waited on and that is all. You cannot say "when this
// finishes, do that" without blocking a thread to find out. Boost.Thread has
// had .then() since 2013, and it is still the clearest illustration of what
// the concurrency TS was trying to add and never landed.

void pillar_continuations() {
    boost::future<int> start = boost::async(boost::launch::async, [] { return 21; });

    // .then() takes the READY FUTURE, not the value -- so the continuation can
    // inspect whether the antecedent held a value or an exception. That is
    // why the lambda's parameter is boost::future<int> and not int.
    boost::future<int> doubled = start.then([](boost::future<int> prev) { return prev.get() * 2; });

    const int value = doubled.get();
    std::printf("continuations: async(21).then(*2) = %d chained=%s\n", value,
                value == 42 ? "yes" : "no");
    report("continuations");
}

void pillar_when_all() {
    boost::future<int> a = boost::async(boost::launch::async, [] { return 1; });
    boost::future<int> b = boost::async(boost::launch::async, [] { return 2; });

    // when_all returns a future holding a tuple of the ORIGINAL futures, so
    // each one's value or exception survives the join individually.
    auto joined = boost::when_all(std::move(a), std::move(b));
    auto results = joined.get();
    const int x = std::get<0>(results).get();
    const int y = std::get<1>(results).get();

    std::printf("when-all: joined two futures -> %d + %d = %d\n", x, y, x + y);
    report("when-all");
}

// ── pillar 2: the upgrade lock ───────────────────────────────────────────
//
// The classic read-mostly pattern: take a shared lock, look at the data, and
// occasionally discover you need to modify it. With std::shared_mutex you
// must drop the shared lock and take a unique one, and in that gap another
// writer can change what you just read -- so you have to re-check everything.
//
// boost::upgrade_lock is a third lock state: shared with respect to other
// readers, exclusive with respect to other UPGRADERS. Promoting it to unique
// never releases, so nothing can slip in between.

void pillar_upgrade() {
    boost::shared_mutex sm;
    int shared_value = 1;
    std::atomic<bool> reader_saw_old{false};
    std::atomic<bool> reader_done{false};

    boost::upgrade_lock<boost::shared_mutex> up(sm);

    // A concurrent reader can still take a shared lock while we hold the
    // UPGRADE lock -- that is the point: readers are not blocked yet.
    boost::thread reader([&] {
        boost::shared_lock<boost::shared_mutex> rd(sm);
        reader_saw_old.store(shared_value == 1, std::memory_order_release);
        reader_done.store(true, std::memory_order_release);
    });
    while (!reader_done.load(std::memory_order_acquire)) {
        boost::this_thread::yield();
    }
    reader.join();

    // Now promote. This does NOT release the lock in between, so no other
    // writer can interleave here.
    {
        boost::upgrade_to_unique_lock<boost::shared_mutex> unique(up);
        shared_value = 2;
    }

    std::printf("upgrade: reader_saw_old_value=%s value_after_promotion=%d\n",
                reader_saw_old.load() ? "yes" : "no", shared_value);
    std::printf("upgrade: promoted shared->unique without releasing\n");
    report("upgrade");
}

// ── pillar 3: interruption, the third cancellation model ─────────────────
//
// ch50: pthread_cancel unwinds through a special exception that glibc
//       requires to propagate. A catch (...) that does not rethrow ABORTS
//       the process -- "FATAL: exception not rethrown", exit 134.
// ch51: std::stop_token sets a flag. Nothing is thrown, so there is nothing
//       to swallow, but a thread that never polls will never stop.
// here: boost::thread::interrupt() throws an ORDINARY C++ exception,
//       boost::thread_interrupted, at defined interruption points.
//
// Ordinary is the operative word, and it is what puts Boost between the two:
// you get an exception you can catch, log, and translate -- and swallowing it
// is a bug rather than a process kill.
//
// The interruption points are a defined list (this_thread::sleep_for,
// condition_variable::wait, thread::join, and interruption_point() among
// others). A tight compute loop contains none of them and will not stop --
// which is a property to know rather than a defect to be surprised by.

void interruptible_worker(std::atomic<bool>* caught) {
    try {
        for (;;) {
            // sleep_for is an interruption point. A busy loop would not be.
            boost::this_thread::sleep_for(boost::chrono::milliseconds(5));
        }
    } catch (const boost::thread_interrupted&) {
        caught->store(true, std::memory_order_release);
        std::printf("interrupt: worker caught boost::thread_interrupted\n");
        std::fflush(stdout);
        // Rethrowing is optional here. Returning is fine -- and that is the
        // whole difference from ch50.
    }
}

void pillar_interrupt() {
    std::atomic<bool> caught{false};
    boost::thread worker(&interruptible_worker, &caught);

    boost::this_thread::sleep_for(boost::chrono::milliseconds(100));
    worker.interrupt();
    worker.join();

    std::printf("interrupt: caught=%s joined_normally=yes\n", caught.load() ? "yes" : "no");
    report("interrupt");
}

void swallowing_worker() {
    try {
        for (;;) {
            boost::this_thread::sleep_for(boost::chrono::milliseconds(5));
        }
    } catch (...) {
        // The EXACT mistake that killed the process in ch50: catch everything,
        // rethrow nothing. Against pthread_cancel's forced unwind this aborts
        // with "FATAL: exception not rethrown". Against a Boost interruption
        // it is merely a swallowed exception, because that is all it is.
        std::printf("interrupt-swallow: caught the interruption and did NOT rethrow\n");
        std::fflush(stdout);
    }
}

// The other half of "defined interruption points": a thread with none of them
// cannot be interrupted at all. This is not a defect -- it is the difference
// between Boost's model and ch50's pthread_cancel, which CAN yank a thread out
// of a compute loop because it is asynchronous. Boost trades that reach for
// predictability: interruption happens where you can see it happening.
//
// The worker below never stops, so this subcommand detaches it and leaves via
// _exit() rather than joining. Everything it touches is static, because a
// detached thread that outlives main() must not be reading main's stack.
std::atomic<bool> g_busy_stopped{false};
volatile unsigned long g_sink = 0;

void busy_worker() {
    try {
        for (unsigned long i = 0;; ++i) {
            g_sink = g_sink + i;  // no sleep, no wait, no interruption point
        }
    } catch (const boost::thread_interrupted&) {
        g_busy_stopped.store(true, std::memory_order_release);
    }
}

void pillar_interrupt_busy() {
    boost::thread worker(&busy_worker);
    boost::this_thread::sleep_for(boost::chrono::milliseconds(100));
    worker.interrupt();
    boost::this_thread::sleep_for(boost::chrono::milliseconds(400));

    const bool stopped = g_busy_stopped.load(std::memory_order_acquire);
    std::printf("interrupt-busy: interrupt() delivered, worker_stopped=%s\n",
                stopped ? "yes" : "no");
    std::printf("interrupt-busy: a loop with no interruption point never checks, "
                "so it never stops\n");
    report("interrupt-busy");

    worker.detach();
    std::fflush(stdout);
    // The worker is still spinning. Returning from main would run static
    // destructors underneath it; leave without unwinding instead.
    _exit(0);
}

void pillar_interrupt_swallow() {
    boost::thread worker(&swallowing_worker);
    boost::this_thread::sleep_for(boost::chrono::milliseconds(100));
    worker.interrupt();
    worker.join();

    // Reaching this line at all is the result. ch50's equivalent never did.
    std::printf("interrupt-swallow: process survived -- boost interruption is an "
                "ordinary exception, not glibc's forced unwind\n");
    report("interrupt-swallow");
}

// ── pillar 4: thread attributes ──────────────────────────────────────────
//
// ch50 set a thread's stack size with pthread_attr_setstacksize, and noted
// that std::thread offers nothing equivalent. Boost.Thread does, portably --
// and on Linux it lands on exactly the same POSIX call, which is why the
// number below matches ch50's to the byte.

std::size_t observed_stack_size() {
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) {
        return 0;
    }
    void* base = nullptr;
    std::size_t size = 0;
    pthread_attr_getstack(&attr, &base, &size);
    pthread_attr_destroy(&attr);
    return size;
}

std::atomic<std::size_t> g_observed{0};

void stack_probe() { g_observed.store(observed_stack_size(), std::memory_order_release); }

void pillar_attributes() {
    constexpr std::size_t kRequested = 256 * 1024;

    boost::thread::attributes attrs;
    attrs.set_stack_size(kRequested);

    // The `const` here is REQUIRED and is not a style choice. With
    // BOOST_THREAD_PROVIDES_VARIADIC_THREAD (default at version 5), a
    // NON-const attributes lvalue is captured by the variadic
    // thread(F&& f, Args&&... args) constructor, which deduces
    // F = thread_attributes and then tries to CALL the attributes object.
    // The result is a wall of template errors out of boost/thread/detail/
    // invoke.hpp that never names the real problem. Binding a const
    // reference makes the thread(attributes const&, F&&) overload the only
    // viable candidate.
    const boost::thread::attributes& cattrs = attrs;
    boost::thread worker(cattrs, &stack_probe);
    worker.join();

    const std::size_t seen = g_observed.load(std::memory_order_acquire);
    std::printf("attributes: requested=%zu observed=%zu at_least_requested=%s\n", kRequested, seen,
                seen >= kRequested ? "yes" : "no");
    std::printf("attributes: this is ch50's pthread_attr_setstacksize, reached portably\n");
    report("attributes");
}

void usage() {
    std::fprintf(stderr,
                 "usage: boostthread <versions|continuations|when-all|upgrade|interrupt|"
                 "interrupt-busy|interrupt-swallow|attributes>\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        usage();
        return 2;
    }
    const std::string pillar = argv[1];

    if (pillar == "versions") {
        pillar_versions();
    } else if (pillar == "continuations") {
        pillar_continuations();
    } else if (pillar == "when-all") {
        pillar_when_all();
    } else if (pillar == "upgrade") {
        pillar_upgrade();
    } else if (pillar == "interrupt") {
        pillar_interrupt();
    } else if (pillar == "interrupt-busy") {
        pillar_interrupt_busy();
    } else if (pillar == "interrupt-swallow") {
        pillar_interrupt_swallow();
    } else if (pillar == "attributes") {
        pillar_attributes();
    } else {
        usage();
        return 2;
    }
    return 0;
}
