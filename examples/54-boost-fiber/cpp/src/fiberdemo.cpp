// fiberdemo -- the reference program for ch54.
//
// ch53 measured a C++20 coroutine frame: 32 bytes, heap-allocated, holding
// only the locals that cross a suspension. A coroutine is STACKLESS.
//
// A Boost fiber is STACKFULL: it owns a real machine stack, switched by
// boost::context. That one difference produces both its cost and its
// capability, and this program measures both.
//
//   stacks       stack_traits and the three allocators, including the guard page
//   versus       the three-way memory table: thread (ch50) / fiber / frame (ch53)
//   deep         a suspend from six frames deep inside ORDINARY functions --
//                the thing a stackless coroutine cannot do at all
//   roundrobin   16 fibers, default scheduler: how many OS threads?
//   sharedwork   the same 16 fibers, work-sharing scheduler: how many now?
//   guard-ok     a fiber that stays within its 64 KiB stack
//   guard-overflow  the same fiber recursing 60x past it -- faults BY DESIGN
//   versions     what Boost reports
//
// guard-overflow dies of SIGSEGV on purpose and is its own subcommand for the
// reason ch50's cancel-swallow and ch51's deadlock-naive were: a subcommand
// that dies must never be able to take the others with it.
//
// Nothing here is timed (see ch39). Every observable is a byte count, a tid
// count, a depth, or an exit signal.

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

#include <boost/context/fixedsize_stack.hpp>
#include <boost/context/protected_fixedsize_stack.hpp>
#include <boost/context/stack_traits.hpp>
#include <boost/fiber/algo/shared_work.hpp>
#include <boost/fiber/all.hpp>
#include <boost/version.hpp>

namespace ctx = boost::context;

namespace {

// ch46-ch53's payload ("The quick brown."), carried forward unchanged.
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
    std::printf("fiber report: case=%s digest=0x%016llx\n", what,
                static_cast<unsigned long long>(fnv1a(kPayload, sizeof kPayload)));
}

// Two numbers this book measured in earlier chapters rather than quoting from
// anywhere. ch50 read a thread's stack with pthread_getattr_np; ch53 read a
// coroutine frame by overloading operator new inside promise_type.
constexpr std::size_t kCh50ThreadStackBytes = 8388608;
constexpr std::size_t kCh53CoroutineFrameBytes = 32;

// The stack every `guard` fiber gets. Small on purpose: big enough for a few
// ordinary frames, far too small for the deliberate overflow.
constexpr std::size_t kGuardStackBytes = 64 * 1024;

// ── stacks ───────────────────────────────────────────────────────────────
//
// boost::context asks a StackAllocator for memory. Three ship with Boost:
//
//   fixedsize_stack            a plain allocation of exactly what you asked
//   protected_fixedsize_stack  the same, plus one PROT_NONE guard page
//   pooled_fixedsize_stack     a pool, for churning many short-lived fibers
//
// The guard page is the interesting one and it is measurable: ask for 64 KiB
// and you are charged 64 KiB + one page, because the extra page exists to be
// unmapped.

void case_stacks() {
    const std::size_t def = ctx::stack_traits::default_size();
    const std::size_t min = ctx::stack_traits::minimum_size();
    const std::size_t page = ctx::stack_traits::page_size();

    std::printf("stacks: default=%zu minimum=%zu page=%zu unbounded=%s\n", def, min, page,
                ctx::stack_traits::is_unbounded() ? "yes" : "no");

    ctx::fixedsize_stack plain(kGuardStackBytes);
    auto plain_sc = plain.allocate();
    const std::size_t plain_size = plain_sc.size;
    plain.deallocate(plain_sc);

    ctx::protected_fixedsize_stack prot(kGuardStackBytes);
    auto prot_sc = prot.allocate();
    const std::size_t prot_size = prot_sc.size;
    prot.deallocate(prot_sc);

    std::printf("stacks: requested=%zu fixedsize=%zu protected=%zu guard_delta=%zu\n",
                kGuardStackBytes, plain_size, prot_size, prot_size - plain_size);
    std::printf("stacks: exact_request=%s guard_is_one_page=%s\n",
                plain_size == kGuardStackBytes ? "yes" : "no",
                (prot_size - plain_size) == page ? "yes" : "no");
    std::printf("stacks: the guard page is mapped PROT_NONE, so running off the end of "
                "the stack faults instead of corrupting whatever is next\n");
    report("stacks");
}

// ── versus: the three-way memory table ───────────────────────────────────

void case_versus() {
    const std::size_t fiber_stack = ctx::stack_traits::default_size();

    const std::size_t vs_thread = fiber_stack > 0 ? kCh50ThreadStackBytes / fiber_stack : 0;
    const std::size_t vs_frame =
        kCh53CoroutineFrameBytes > 0 ? fiber_stack / kCh53CoroutineFrameBytes : 0;

    std::printf("versus: thread=%zu fiber=%zu coroutine_frame=%zu\n", kCh50ThreadStackBytes,
                fiber_stack, kCh53CoroutineFrameBytes);
    std::printf("versus: fibers_per_thread_stack=%zu frames_per_fiber_stack=%zu\n", vs_thread,
                vs_frame);
    std::printf("versus: fiber_between_the_two=%s\n",
                (fiber_stack > kCh53CoroutineFrameBytes && fiber_stack < kCh50ThreadStackBytes)
                    ? "yes"
                    : "no");
    std::printf("versus: a fiber costs more than a frame because it HAS a stack, and less "
                "than a thread because the kernel never sees it\n");
    report("versus");
}

// ── deep: what the stack actually buys ───────────────────────────────────
//
// This is the functional difference, not a performance one. `leaf` is an
// ordinary function. So is `level`. Neither is a coroutine, and neither
// returns anything special -- yet the fiber suspends from inside `leaf`, six
// frames below where the fiber body started, and resumes exactly there.
//
// A stackless coroutine cannot do this at any price. `co_await` is only valid
// in the body of a coroutine, so every frame between the suspension and the
// scheduler must itself be a coroutine -- the "function colouring" problem.
// Suspending from a plain function nested in a plain function is the one
// thing 128 KiB of stack buys that 32 bytes of frame cannot.

int g_depth = 0;
int g_yield_depth = 0;
int g_resume_depth = 0;

void leaf() {
    g_yield_depth = g_depth;
    boost::this_fiber::yield();  // suspend from an ORDINARY function
    g_resume_depth = g_depth;
}

void level(int remaining) {
    ++g_depth;
    if (remaining == 0) {
        leaf();
    } else {
        level(remaining - 1);
    }
}

void case_deep() {
    g_depth = g_yield_depth = g_resume_depth = 0;
    boost::fibers::fiber f([] { level(5); });
    f.join();

    std::printf("deep: yielded_at_depth=%d resumed_at_depth=%d same=%s\n", g_yield_depth,
                g_resume_depth, g_yield_depth == g_resume_depth ? "yes" : "no");
    std::printf("deep: suspended from a plain function %d frames below the fiber body -- "
                "no co_await, no coroutine, no colouring\n",
                g_yield_depth);
    report("deep");
}

// ── M:N, and the scheduler is yours to pick ──────────────────────────────
//
// ch44 covered Go's answer: the GMP runtime decides, automatically, and you
// mostly cannot influence it. C++ makes the choice explicit and observable.
//
// Both cases below run the SAME 16 fibers doing the SAME work. The only
// difference is one call to use_scheduling_algorithm, and the observable is
// how many OS threads those fibers were seen running on -- gettid(), the same
// kernel task id ch50 established.

constexpr int kFibers = 16;
constexpr int kYieldsPerFiber = 50;
constexpr int kWorkerThreads = 4;

std::mutex g_tid_mutex;
std::set<long> g_tids;

void note_tid() {
    const std::lock_guard<std::mutex> lock(g_tid_mutex);
    g_tids.insert(static_cast<long>(gettid()));
}

void fiber_body() {
    for (int i = 0; i < kYieldsPerFiber; ++i) {
        note_tid();
        boost::this_fiber::yield();
    }
}

void case_roundrobin() {
    g_tids.clear();
    {
        std::vector<boost::fibers::fiber> fibers;
        fibers.reserve(kFibers);
        for (int i = 0; i < kFibers; ++i) {
            fibers.emplace_back(&fiber_body);
        }
        for (auto& f : fibers) {
            f.join();
        }
    }
    std::printf("roundrobin: fibers=%d distinct_tids=%zu\n", kFibers, g_tids.size());
    std::printf("roundrobin: the default scheduler never migrates a fiber -- 16 tasks in "
                "flight, one CPU occupied. ch49 called this concurrent, not parallel.\n");
    report("roundrobin");
}

void case_sharedwork() {
    g_tids.clear();
    boost::fibers::barrier finish(kWorkerThreads);

    std::vector<std::thread> threads;
    threads.reserve(kWorkerThreads);
    for (int t = 0; t < kWorkerThreads; ++t) {
        threads.emplace_back([t, &finish] {
            // Every participating thread must opt in. shared_work keeps ONE
            // queue that all of them pull from, so a fiber can resume on a
            // different thread than the one it suspended on.
            boost::fibers::use_scheduling_algorithm<boost::fibers::algo::shared_work>();

            if (t == 0) {
                std::vector<boost::fibers::fiber> fibers;
                fibers.reserve(kFibers);
                for (int i = 0; i < kFibers; ++i) {
                    fibers.emplace_back(&fiber_body);
                }
                for (auto& f : fibers) {
                    f.join();
                }
            }
            finish.wait();
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    std::printf("sharedwork: fibers=%d worker_threads=%d distinct_tids=%zu\n", kFibers,
                kWorkerThreads, g_tids.size());
    std::printf("sharedwork: same fibers, same work -- one call to "
                "use_scheduling_algorithm and they migrate across threads\n");
    report("sharedwork");
}

// ── guard: the page that turns corruption into a fault ───────────────────
//
// Each frame of `burn_stack` parks 4 KiB on the stack and touches it, so the
// recursion consumes stack in a way the compiler cannot reason away. `volatile`
// and the write are both load-bearing: without them an optimizing build can
// drop the array, turn the recursion into a loop, and never approach the
// guard page at all. This translation unit is compiled -O0 for the same
// reason (see CMakeLists.txt).

void burn_stack(int remaining) {
    volatile char pad[4096];
    pad[0] = static_cast<char>(remaining);
    if (remaining > 0) {
        burn_stack(remaining - 1);
    }
    (void)pad[0];
}

// 5 frames * ~4 KiB is comfortably inside 64 KiB.
constexpr int kInBudgetFrames = 5;
// 1000 frames is ~4 MiB against a 64 KiB stack -- 60x past it, so this cannot
// accidentally fit on a host with different frame padding.
constexpr int kOverflowFrames = 1000;

void case_guard_ok() {
    boost::fibers::fiber f(std::allocator_arg, ctx::protected_fixedsize_stack(kGuardStackBytes),
                           [] { burn_stack(kInBudgetFrames); });
    f.join();
    std::printf("guard-ok: %d frames on a %zu-byte protected stack completed normally\n",
                kInBudgetFrames, kGuardStackBytes);
    report("guard-ok");
}

void case_guard_overflow() {
    std::printf("guard-overflow: recursing %d frames on a %zu-byte protected stack; "
                "this is expected to die on the guard page\n",
                kOverflowFrames, kGuardStackBytes);
    std::fflush(stdout);

    boost::fibers::fiber f(std::allocator_arg, ctx::protected_fixedsize_stack(kGuardStackBytes),
                           [] { burn_stack(kOverflowFrames); });
    f.join();

    // Not reached. Whatever a process prints after running off its stack is
    // undefined, so nothing asserts on this line -- the gate checks the exit
    // signal instead, exactly as ch49's oob_unchecked was gated statically
    // rather than on its output.
    std::printf("guard-overflow: SURVIVED -- the guard page did not fault\n");
}

void case_versions() {
    std::printf("versions: Boost %d.%d.%d\n", BOOST_VERSION / 100000, (BOOST_VERSION / 100) % 1000,
                BOOST_VERSION % 100);
    std::printf("versions: fiber_default_stack=%zu context_page_size=%zu\n",
                ctx::stack_traits::default_size(), ctx::stack_traits::page_size());
    report("versions");
}

void usage() {
    std::fprintf(stderr, "usage: fiberdemo <versions|stacks|versus|deep|roundrobin|sharedwork|"
                         "guard-ok|guard-overflow>\n");
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
    } else if (what == "stacks") {
        case_stacks();
    } else if (what == "versus") {
        case_versus();
    } else if (what == "deep") {
        case_deep();
    } else if (what == "roundrobin") {
        case_roundrobin();
    } else if (what == "sharedwork") {
        case_sharedwork();
    } else if (what == "guard-ok") {
        case_guard_ok();
    } else if (what == "guard-overflow") {
        case_guard_overflow();
    } else {
        usage();
        return 2;
    }
    return 0;
}
