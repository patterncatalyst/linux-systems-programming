// conc -- the reference program for ch49 (concurrency vs parallelism).
//
// One workload, four execution models, and observables that report STRUCTURE
// rather than elapsed time:
//
//   model       tasks in flight   CPUs occupied   reading
//   sequential  1                 1               neither concurrent nor parallel
//   concurrent  N                 1 (pinned)      concurrent, NOT parallel
//   parallel    N (= cores)       many            parallel, minimal task sharing
//   both        2N (oversub)      many            concurrent AND parallel
//
// Every model computes the identical digest. That is the chapter's thesis:
// the execution model is a property of the machine, not of the answer.
//
// Nothing here is timed. Wall-clock numbers are not reproducible and this
// book does not gate on them (see ch39); the gates below are counts and set
// cardinalities, which are.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sched.h>

namespace {

// ch46/ch47/ch48's exact 16-byte payload ("The quick brown."), reused
// byte-for-byte so this chapter lands on the same FNV-1a digest as the three
// toolbox appendices -- now carried into Part 14.
constexpr std::uint8_t kPayload[16] = {0x54, 0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63,
                                       0x6b, 0x20, 0x62, 0x72, 0x6f, 0x77, 0x6e, 0x2e};

constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x00000100000001b3ULL;

// Integer-only, no floats, addresses, timing, or hash-map iteration -- the
// same input bytes produce the same digest on every conforming toolchain.
std::uint64_t fnv1a(const std::uint8_t* data, std::size_t len) {
    std::uint64_t h = kFnvOffsetBasis;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= kFnvPrime;  // uint64_t wraps by definition -- no UB to sanitize
    }
    return h;
}

// How many sample points each worker takes, and how much CPU it burns between
// two of them. Both are large enough that a worker is descheduled and resumed
// many times over its life (which is what makes interleaving observable) and
// small enough that the whole demo is a fraction of a second.
constexpr int kSamplesPerWorker = 240;
constexpr int kBurnPerSample = 900;

// Shared observation state. Every counter here is a structural fact about how
// the run was executed; none of them is a duration.
struct Observations {
    // Monotonic ticket handed out at every sample point by every worker. A
    // worker whose consecutive tickets are NOT consecutive integers was
    // interleaved with some other worker in between -- that gap is the
    // definition of "in flight at the same time" this program uses.
    std::atomic<std::uint64_t> ticket{0};
    std::atomic<std::uint64_t> interleaves{0};

    // Live worker count and its high-water mark: "tasks in flight".
    std::atomic<int> inflight{0};
    std::atomic<int> max_inflight{0};

    // Bitmap of every CPU any worker was observed running on, unioned across
    // all workers: "CPUs occupied".
    std::atomic<std::uint64_t> cpu_mask{0};

    // Set to false if any worker disagrees about the digest.
    std::atomic<bool> consensus{true};
    std::atomic<std::uint64_t> digest{0};

    // Where each worker's burn-loop result goes to die. A burn loop whose
    // result nobody reads is dead code, and a compiler that deletes it leaves
    // workers finishing in microseconds, never overlapping -- so `concurrent`
    // reports max_inflight=1 interleaves=0, the signature of a sequential run,
    // no matter which model was asked for. Measured on this host at -O2, for
    // the `concurrent` model over three runs each:
    //
    //   burn-loop body        sink      g++ 16.1.1       clang++ 22.1.8
    //   serial chain          present   inflight 16      inflight 16   <- shipped
    //   serial chain          absent    inflight 16      inflight 1
    //   loop-invariant        present   inflight 1-2     inflight 1
    //   loop-invariant        absent    inflight 1       inflight 1
    //
    // Neither the sink nor the dependency chain in the loop body is sufficient
    // alone: drop either one and at least one of the two compilers optimizes
    // the workload away. Together they are the difference between measuring
    // the scheduler and measuring the optimizer.
    std::atomic<std::uint64_t> sink{0};
};

void bump_max_inflight(Observations& obs, int now) {
    int seen = obs.max_inflight.load(std::memory_order_relaxed);
    while (now > seen && !obs.max_inflight.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
        // compare_exchange_weak refreshes `seen` on failure; loop until either
        // we win the race or someone else already recorded a larger mark.
    }
}

// One unit of work. Every worker runs exactly this, in every model -- the
// models differ only in how the workers are scheduled onto the machine.
void worker(Observations& obs, int /*id*/) {
    bump_max_inflight(obs, obs.inflight.fetch_add(1, std::memory_order_relaxed) + 1);

    std::uint64_t local = 0;
    std::uint64_t prev_ticket = 0;
    bool have_prev = false;

    for (int s = 0; s < kSamplesPerWorker; ++s) {
        // Burn a deterministic amount of CPU so the worker is a real, runnable
        // task the scheduler has to place somewhere. Each iteration hashes the
        // previous iteration's value, so this is a genuine serial dependency
        // chain: the optimizer cannot hoist it, collapse it, or vectorize it.
        // An earlier version hashed a loop-invariant constant instead, and
        // both compilers reduced the whole thing to almost nothing -- see the
        // table on Observations::sink above.
        for (int b = 0; b < kBurnPerSample; ++b) {
            local = fnv1a(reinterpret_cast<const std::uint8_t*>(&local), sizeof local) + b;
        }

        const std::uint64_t t = obs.ticket.fetch_add(1, std::memory_order_relaxed);
        if (have_prev && t != prev_ticket + 1) {
            obs.interleaves.fetch_add(1, std::memory_order_relaxed);
        }
        prev_ticket = t;
        have_prev = true;

        // sched_getcpu() is a vDSO read of the CPU this thread is on right
        // now. It can change between this line and the next -- that is the
        // point. We union every value ever seen rather than trusting one.
        const int cpu = sched_getcpu();
        if (cpu >= 0 && cpu < 64) {
            obs.cpu_mask.fetch_or(std::uint64_t{1} << cpu, std::memory_order_relaxed);
        }
    }

    // Consume the burn-loop result so it cannot be optimized out. The value
    // itself is meaningless and is never asserted on -- only its existence
    // matters, and xor keeps the sink from depending on completion order.
    obs.sink.fetch_xor(local, std::memory_order_relaxed);

    // Every worker independently derives the same digest from the same bytes.
    const std::uint64_t d = fnv1a(kPayload, sizeof kPayload);
    std::uint64_t expect = 0;
    if (!obs.digest.compare_exchange_strong(expect, d, std::memory_order_relaxed) && expect != d) {
        obs.consensus.store(false, std::memory_order_relaxed);
    }

    obs.inflight.fetch_sub(1, std::memory_order_relaxed);
}

int cpus_allowed() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) != 0) {
        return -1;
    }
    return CPU_COUNT(&set);
}

// Restrict this process to a single CPU. This is what turns "N runnable
// threads" into "N runnable threads that must take turns" -- concurrency
// with the parallelism removed, by request rather than by accident.
bool pin_to_one_cpu() {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    return sched_setaffinity(0, sizeof set, &set) == 0;
}

int popcount64(std::uint64_t v) {
    int n = 0;
    for (; v; v &= v - 1) {
        ++n;
    }
    return n;
}

void run_threaded(Observations& obs, int workers) {
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(workers));
    for (int i = 0; i < workers; ++i) {
        threads.emplace_back(worker, std::ref(obs), i);
    }
    for (auto& t : threads) {
        t.join();
    }
}

// The sequential model deliberately does NOT spawn threads: each worker runs
// to completion on this thread before the next one starts. That is what makes
// its interleaves count exactly zero -- one worker's tickets are never
// separated by another's.
void run_sequential(Observations& obs, int workers) {
    for (int i = 0; i < workers; ++i) {
        worker(obs, i);
    }
}

void usage() {
    std::fprintf(stderr, "usage: conc --model <sequential|concurrent|parallel|both>\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string model;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if (model.empty()) {
        usage();
        return 2;
    }

    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    const int n = cores > 0 ? cores : 4;

    Observations obs;
    int workers = 0;

    if (model == "sequential") {
        // One task at a time on one thread: neither concurrent nor parallel.
        workers = 4;
        run_sequential(obs, workers);
    } else if (model == "concurrent") {
        // N runnable threads, one CPU allowed: concurrent, NOT parallel.
        if (!pin_to_one_cpu()) {
            std::fprintf(stderr, "conc: sched_setaffinity failed\n");
            return 1;
        }
        workers = n;
        run_threaded(obs, workers);
    } else if (model == "parallel") {
        // One thread per CPU, unpinned: the tasks need not share a CPU.
        workers = n;
        run_threaded(obs, workers);
    } else if (model == "both") {
        // Twice as many threads as CPUs, unpinned: tasks share CPUs AND
        // occupy many of them at once.
        workers = 2 * n;
        run_threaded(obs, workers);
    } else {
        usage();
        return 2;
    }

    const int allowed = cpus_allowed();
    const int distinct = popcount64(obs.cpu_mask.load());

    std::printf(
        "conc report: model=%s workers=%d cpus_allowed=%d distinct_cpus=%d "
        "max_inflight=%d interleaves=%llu consensus=%s digest=0x%016llx\n",
        model.c_str(), workers, allowed, distinct, obs.max_inflight.load(),
        static_cast<unsigned long long>(obs.interleaves.load()),
        obs.consensus.load() ? "yes" : "no",
        static_cast<unsigned long long>(obs.digest.load()));

    return obs.consensus.load() ? 0 : 1;
}
