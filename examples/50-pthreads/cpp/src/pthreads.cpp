// pthreads -- the reference program for ch50.
//
// ch49 measured concurrency and parallelism through std::thread. This program
// goes under std::thread to the thing it actually is on Linux: a pthread. Each
// subcommand exercises one per-thread control that POSIX offers and the C++
// standard library does not expose, and reports an observable the kernel or
// glibc produced:
//
//   identity  pthread_t vs gettid() vs getpid(), and /proc/self/task/
//   naming    pthread_setname_np -> /proc/self/task/<tid>/comm
//   stack     pthread_attr_setstacksize -> pthread_getattr_np
//   affinity  pthread_setaffinity_np, per THREAD (ch49 pinned the process)
//   sched     pthread_getschedparam, and what SCHED_FIFO costs unprivileged
//   cancel    pthread_cancel, C++ unwinding, and why stop_token exists
//
// One subcommand per facet, deliberately: the cancel facet's second case ends
// in abort(), and a facet that aborts must never be able to take the others
// with it.
//
// Every facet prints the same digest. As in ch46-ch49, the answer does not
// depend on how the work was scheduled -- or, here, on which per-thread knob
// was turned.

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

namespace {

// ch46/ch47/ch48/ch49's exact 16-byte payload ("The quick brown."), carried
// forward so every chapter in the book lands on the same FNV-1a digest.
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

void report(const char* facet) {
    std::printf("pthreads report: facet=%s digest=0x%016llx\n", facet,
                static_cast<unsigned long long>(fnv1a(kPayload, sizeof kPayload)));
}

// The kernel's name for a task, straight from procfs. This is the same string
// `top -H`, `ps -L`, and gdb's `info threads` display -- reading it here proves
// the name reached the kernel rather than living in a glibc-side variable.
std::string comm_of(pid_t tid) {
    std::ifstream f("/proc/self/task/" + std::to_string(tid) + "/comm");
    std::string s;
    std::getline(f, s);
    return s;
}

bool task_dir_exists(pid_t tid) {
    return access(("/proc/self/task/" + std::to_string(tid)).c_str(), F_OK) == 0;
}

int cpus_allowed() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) != 0) {
        return -1;
    }
    return CPU_COUNT(&set);
}

// ── identity ─────────────────────────────────────────────────────────────
//
// A Linux thread has two identities and they are not interchangeable.
// pthread_t is an opaque glibc handle -- you may compare it with
// pthread_equal and nothing else; it is not a number and printing it is not
// portable. gettid() is the kernel's task id: a real integer, the thing that
// names a directory in /proc, the thing perf and ftrace and gdb report.
//
// The main thread is the case that surprises people: its tid EQUALS the
// process's pid, because a "process" on Linux is its first thread.

void facet_identity() {
    std::printf("identity: main pid=%ld tid=%ld same=%s\n", static_cast<long>(getpid()),
                static_cast<long>(gettid()), getpid() == gettid() ? "yes" : "no");

    constexpr int kWorkers = 3;
    std::vector<pid_t> tids(kWorkers, 0);
    std::vector<std::thread> threads;
    for (int i = 0; i < kWorkers; ++i) {
        threads.emplace_back([&tids, i] { tids[static_cast<std::size_t>(i)] = gettid(); });
    }
    for (auto& t : threads) {
        t.join();
    }

    // Distinct from each other and from the process -- and each one named a
    // real directory under /proc/self/task while it was alive.
    bool distinct = true;
    for (int i = 0; i < kWorkers; ++i) {
        if (tids[static_cast<std::size_t>(i)] == getpid()) {
            distinct = false;
        }
        for (int j = i + 1; j < kWorkers; ++j) {
            if (tids[static_cast<std::size_t>(i)] == tids[static_cast<std::size_t>(j)]) {
                distinct = false;
            }
        }
        std::printf("identity: worker %d tid=%ld\n", i, static_cast<long>(tids[static_cast<std::size_t>(i)]));
    }
    std::printf("identity: worker_tids_distinct=%s main_task_dir=%s\n", distinct ? "yes" : "no",
                task_dir_exists(gettid()) ? "present" : "absent");
    report("identity");
}

// ── naming ───────────────────────────────────────────────────────────────
//
// std::thread has no name. pthread_setname_np does, and the name it sets is
// not a glibc bookkeeping detail: it lands in the kernel's task_struct and
// shows up in /proc/self/task/<tid>/comm, in `top -H`, and in gdb.
//
// The size limit is the interesting part. The kernel's comm field is 16 bytes
// INCLUDING the terminating NUL, so 15 characters is the most that fits.
// glibc refuses a longer name with ERANGE rather than truncating it, which
// means an over-long name leaves the previous one in place -- a silent no-op
// if you do not check the return value.

void facet_naming() {
    pid_t tid = 0;
    std::string via_pthread;
    std::string via_proc;
    int too_long_rc = 0;
    std::string after_too_long;

    std::thread t([&] {
        tid = gettid();
        pthread_setname_np(pthread_self(), "ch50-worker");

        char buf[32] = {};
        pthread_getname_np(pthread_self(), buf, sizeof buf);
        via_pthread = buf;
        via_proc = comm_of(tid);

        // 16 characters: one too many for a 16-byte field that must hold a NUL.
        too_long_rc = pthread_setname_np(pthread_self(), "0123456789abcdef");
        after_too_long = comm_of(tid);
    });
    t.join();

    std::printf("naming: tid=%ld via_pthread='%s' via_proc='%s' agree=%s\n", static_cast<long>(tid),
                via_pthread.c_str(), via_proc.c_str(), via_pthread == via_proc ? "yes" : "no");
    std::printf("naming: 16char_rc=%d (%s) name_after='%s' unchanged=%s\n", too_long_rc,
                too_long_rc == ERANGE ? "ERANGE" : std::strerror(too_long_rc), after_too_long.c_str(),
                after_too_long == via_proc ? "yes" : "no");
    report("naming");
}

// ── stack ────────────────────────────────────────────────────────────────
//
// std::thread gives you no say in stack size whatsoever. You get the default,
// which on glibc is RLIMIT_STACK for the main thread and a flat 8 MiB for
// every thread it creates. For a program that spawns thousands of threads
// that default is the difference between fitting in memory and not.
//
// pthread_attr_setstacksize sets it, pthread_getattr_np reads back what was
// actually installed, and a request below PTHREAD_STACK_MIN is refused with
// EINVAL rather than quietly rounded up.

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

void* stack_probe(void* out) {
    *static_cast<std::size_t*>(out) = observed_stack_size();
    return nullptr;
}

void facet_stack() {
    constexpr std::size_t kRequested = 256 * 1024;

    std::size_t default_size = 0;
    pthread_t d{};
    pthread_create(&d, nullptr, stack_probe, &default_size);
    pthread_join(d, nullptr);

    std::size_t custom_size = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    const int set_rc = pthread_attr_setstacksize(&attr, kRequested);
    pthread_t c{};
    pthread_create(&c, &attr, stack_probe, &custom_size);
    pthread_join(c, nullptr);
    pthread_attr_destroy(&attr);

    // Below the floor: refused, not rounded.
    pthread_attr_t small;
    pthread_attr_init(&small);
    const int too_small_rc = pthread_attr_setstacksize(&small, 8192);
    pthread_attr_destroy(&small);

    std::printf("stack: main=%zu default_worker=%zu custom=%zu requested=%zu set_rc=%d\n",
                observed_stack_size(), default_size, custom_size, kRequested, set_rc);
    std::printf("stack: custom_smaller_than_default=%s custom_at_least_requested=%s\n",
                custom_size < default_size ? "yes" : "no", custom_size >= kRequested ? "yes" : "no");
    std::printf("stack: PTHREAD_STACK_MIN=%ld below_min_rc=%d (%s)\n", static_cast<long>(PTHREAD_STACK_MIN),
                too_small_rc, too_small_rc == EINVAL ? "EINVAL" : std::strerror(too_small_rc));
    report("stack");
}

// ── affinity ─────────────────────────────────────────────────────────────
//
// ch49 called sched_setaffinity(0, ...) and moved the WHOLE PROCESS onto one
// CPU. pthread_setaffinity_np moves ONE THREAD, which is the control you
// actually want for a pinned fast path beside unpinned background work
// (ch40's territory). The observable difference: two workers pinned to two
// different CPUs report two different sched_getcpu() values, while the main
// thread's own affinity mask is left exactly as it was.

struct PinArg {
    int cpu;
    int observed;
    int rc;
};

void* pin_probe(void* raw) {
    auto* a = static_cast<PinArg*>(raw);
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(a->cpu), &set);
    a->rc = pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    // sched_yield gives the kernel an obvious moment to honour the new mask;
    // it is not required for correctness, only for a prompt observation.
    sched_yield();
    a->observed = sched_getcpu();
    return nullptr;
}

void facet_affinity() {
    const int allowed_before = cpus_allowed();
    if (allowed_before < 2) {
        std::printf("affinity: cpus_allowed=%d -- need at least 2 to pin two threads apart\n",
                    allowed_before);
        report("affinity");
        return;
    }

    PinArg a{0, -1, -1};
    PinArg b{1, -1, -1};
    pthread_t ta{};
    pthread_t tb{};
    pthread_create(&ta, nullptr, pin_probe, &a);
    pthread_create(&tb, nullptr, pin_probe, &b);
    pthread_join(ta, nullptr);
    pthread_join(tb, nullptr);

    const int allowed_after = cpus_allowed();
    std::printf("affinity: worker_a asked=%d rc=%d observed_cpu=%d\n", a.cpu, a.rc, a.observed);
    std::printf("affinity: worker_b asked=%d rc=%d observed_cpu=%d\n", b.cpu, b.rc, b.observed);
    std::printf("affinity: workers_on_different_cpus=%s main_cpus_allowed=%d unchanged=%s\n",
                (a.observed != b.observed && a.observed >= 0 && b.observed >= 0) ? "yes" : "no",
                allowed_after, allowed_before == allowed_after ? "yes" : "no");
    report("affinity");
}

// ── sched ────────────────────────────────────────────────────────────────
//
// Scheduling policy is per-thread and std::thread cannot touch it. The
// default is SCHED_OTHER at priority 0 -- the fair scheduler, where "priority"
// is nice and the sched_param priority is required to be zero.
//
// Asking for SCHED_FIFO is the interesting call precisely because it FAILS
// here: real-time policy needs CAP_SYS_NICE or an RLIMIT_RTPRIO allowance, and
// an unprivileged process gets EPERM. That refusal is the observable. A demo
// that required root to show anything would not run on a reader's laptop.

void facet_sched() {
    int policy = -1;
    sched_param param{};
    const int get_rc = pthread_getschedparam(pthread_self(), &policy, &param);

    sched_param rt{};
    rt.sched_priority = 50;
    const int fifo_rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &rt);

    int policy_after = -1;
    sched_param after{};
    pthread_getschedparam(pthread_self(), &policy_after, &after);

    std::printf("sched: get_rc=%d policy=%d is_other=%s priority=%d\n", get_rc, policy,
                policy == SCHED_OTHER ? "yes" : "no", param.sched_priority);
    std::printf("sched: SCHED_FIFO prio 50 -> rc=%d (%s) policy_after=%d unchanged=%s\n", fifo_rc,
                fifo_rc == EPERM ? "EPERM" : std::strerror(fifo_rc), policy_after,
                policy == policy_after ? "yes" : "no");
    report("sched");
}

// ── cancel ───────────────────────────────────────────────────────────────
//
// pthread_cancel is the facility C++ deliberately did NOT standardize, and
// this facet shows why by running it against C++ code.
//
// Case 1 is the part that works better than its reputation: on glibc,
// cancellation unwinds the stack, so C++ destructors DO run and
// pthread_cleanup_push handlers DO run. RAII is not silently skipped.
//
// Case 2 is the trap. The unwind is implemented as a special exception, and
// glibc requires it to propagate. A `catch (...)` that does not rethrow
// swallows it, and glibc responds by aborting the whole process with
// "FATAL: exception not rethrown". A perfectly ordinary, correct-looking C++
// catch-all turns a thread cancellation into a dead process.
//
// C++20's std::jthread and std::stop_token exist to avoid all of this: a
// cooperative request the target checks when it chooses to, with no unwinding
// and no exception the caller must be careful not to catch.

struct Guard {
    const char* name;
    ~Guard() { std::printf("cancel: ~Guard(%s) ran during unwinding\n", name); }
};

void cleanup_handler(void* arg) {
    std::printf("cancel: pthread_cleanup handler ran (holding %s)\n", static_cast<const char*>(arg));
    std::fflush(stdout);
}

void* unwind_worker(void*) {
    pthread_cleanup_push(cleanup_handler, const_cast<char*>("a-resource"));
    Guard g{"raii"};
    std::printf("cancel: worker parked on a cancellation point\n");
    std::fflush(stdout);
    for (;;) {
        sleep(1);  // sleep(3) is a cancellation point
    }
    pthread_cleanup_pop(1);
    return nullptr;
}

void* swallow_worker(void*) {
    try {
        std::printf("cancel: worker parked inside catch (...)\n");
        std::fflush(stdout);
        for (;;) {
            sleep(1);
        }
    } catch (...) {
        // Looks like ordinary defensive C++. It is fatal here: the forced
        // unwind must be allowed to propagate, and glibc aborts if it is not.
        std::printf("cancel: caught the forced unwind and did not rethrow\n");
        std::fflush(stdout);
    }
    return nullptr;
}

void facet_cancel(bool swallow) {
    pthread_t t{};
    if (swallow) {
        // This case ends in abort() by design. It is its own subcommand so it
        // can never take the other facets down with it.
        pthread_create(&t, nullptr, swallow_worker, nullptr);
        usleep(200000);
        pthread_cancel(t);
        pthread_join(t, nullptr);
        std::printf("cancel: still alive after swallowing the unwind\n");
        return;
    }

    pthread_create(&t, nullptr, unwind_worker, nullptr);
    usleep(200000);
    const int rc = pthread_cancel(t);
    void* retval = nullptr;
    pthread_join(t, &retval);
    std::printf("cancel: pthread_cancel rc=%d joined_retval_is_canceled=%s\n", rc,
                retval == PTHREAD_CANCELED ? "yes" : "no");
    report("cancel");
}

// ── the bridge ───────────────────────────────────────────────────────────
//
// Everything above used raw pthread_create, but none of it has to. A
// std::thread IS a pthread here -- native_handle_type is pthread_t, checked
// at compile time below -- so the standard type and the POSIX controls
// compose. This is the sanctioned escape hatch, and using it is not a hack:
// native_handle() exists in the standard precisely so that platform facilities
// the standard does not wrap remain reachable.

static_assert(std::is_same_v<std::thread::native_handle_type, pthread_t>,
              "on this platform std::thread::native_handle() is a pthread_t");

void facet_bridge() {
    pid_t tid = 0;
    std::string name;
    std::atomic<bool> named{false};

    std::thread t([&] {
        tid = gettid();
        while (!named.load(std::memory_order_acquire)) {
            sched_yield();
        }
        name = comm_of(gettid());
    });

    // Name a std::thread from the outside, through its native handle.
    const int rc = pthread_setname_np(t.native_handle(), "std-thread-x");
    named.store(true, std::memory_order_release);
    t.join();

    std::printf("bridge: native_handle_is_pthread_t=yes setname_rc=%d tid=%ld proc_comm='%s'\n", rc,
                static_cast<long>(tid), name.c_str());
    report("bridge");
}

void usage() {
    std::fprintf(stderr,
                 "usage: pthreads <identity|naming|stack|affinity|sched|cancel|"
                 "cancel-swallow|bridge>\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        usage();
        return 2;
    }
    const std::string facet = argv[1];

    if (facet == "identity") {
        facet_identity();
    } else if (facet == "naming") {
        facet_naming();
    } else if (facet == "stack") {
        facet_stack();
    } else if (facet == "affinity") {
        facet_affinity();
    } else if (facet == "sched") {
        facet_sched();
    } else if (facet == "cancel") {
        facet_cancel(false);
    } else if (facet == "cancel-swallow") {
        facet_cancel(true);
    } else if (facet == "bridge") {
        facet_bridge();
    } else {
        usage();
        return 2;
    }
    return 0;
}
