// coro -- the reference program for ch53.
//
// Chapter 27 already built a working C++20 coroutine engine: promise_type,
// the awaitable protocol, await_suspend returning a handle for symmetric
// transfer, and an epoll reactor that parks and resumes handles. That chapter
// owns the MECHANISM, and nothing here repeats it.
//
// This program asks the question ch27 never did: what does a suspended
// computation COST?
//
// The answer connects directly to ch50, which measured a thread's stack at
// 8388608 bytes. A coroutine frame holding the same "paused work" is measured
// below in tens of bytes. That ratio is why coroutines exist.
//
//   frames         frame size for three shapes, via operator new in the promise
//   halo           does the compiler elide the allocation? (it depends, and
//                  the answer differs between GCC and clang on this host)
//   generator      C++23 std::generator -- ch27 hand-rolled one, C++23 ships it
//   versus-thread  frame bytes against ch50's measured 8 MiB thread stack
//   lifetime       the trap: a frame is not owned by anything that scopes
//   versions       what the toolchain reports
//
// Nothing here is timed (see ch39). Every observable is a byte count, an
// allocation count, or a computed value.

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <generator>
#include <new>
#include <string>
#include <version>

namespace {

// ch46-ch52's payload ("The quick brown."), carried forward unchanged.
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
    std::printf("coro report: case=%s digest=0x%016llx\n", what,
                static_cast<unsigned long long>(fnv1a(kPayload, sizeof kPayload)));
}

// ch50 measured this on this host: a created thread's stack is exactly 8 MiB.
// It is a constant here so the comparison in `versus-thread` cites a real
// prior measurement rather than a round number someone remembered.
constexpr std::size_t kCh50ThreadStackBytes = 8388608;

// ── instrumentation ──────────────────────────────────────────────────────
//
// A coroutine frame is allocated by the compiler, not by you -- unless the
// promise type declares operator new, in which case every frame allocation
// comes through it. That is the only portable way to see the size of a thing
// the language deliberately hides.

std::size_t g_last_frame = 0;
std::size_t g_alloc_count = 0;
std::size_t g_free_count = 0;

struct Counted {
    struct promise_type {
        static void* operator new(std::size_t n) {
            g_last_frame = n;
            ++g_alloc_count;
            return ::operator new(n);
        }
        static void operator delete(void* p, std::size_t) noexcept {
            ++g_free_count;
            ::operator delete(p);
        }

        Counted get_return_object() {
            return Counted{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        // suspend_always at initial: the coroutine does not start until it is
        // resumed, so the frame provably outlives this call and there is
        // nothing for the optimizer to elide. `frames` wants the allocation.
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };

    std::coroutine_handle<promise_type> handle;

    // A coroutine handle is NOT an owning type. If nothing calls destroy(),
    // the frame leaks -- see the `lifetime` case.
    ~Counted() {
        if (handle) {
            handle.destroy();
        }
    }
    Counted(const Counted&) = delete;
    Counted& operator=(const Counted&) = delete;
    explicit Counted(std::coroutine_handle<promise_type> h) : handle(h) {}
};

// ── frames: what is actually in there ────────────────────────────────────
//
// The frame holds the promise, the resume/destroy function pointers, a state
// index, and every local that is LIVE ACROSS A SUSPENSION. A local used only
// between two co_awaits, or only before the first one, need not be stored --
// which is why frame size tracks what you carry, not how much you declare.

Counted frame_trivial() { co_await std::suspend_always{}; }

Counted frame_small() {
    int a = 1, b = 2, c = 3, d = 4;
    char buf[256]{};
    co_await std::suspend_always{};  // everything above must survive this
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)buf[0];
}

Counted frame_large() {
    char buf[4096]{};
    co_await std::suspend_always{};
    (void)buf[0];
}

void case_frames() {
    std::size_t trivial = 0, small = 0, large = 0;
    {
        Counted c = frame_trivial();
        trivial = g_last_frame;
    }
    {
        Counted c = frame_small();
        small = g_last_frame;
    }
    {
        Counted c = frame_large();
        large = g_last_frame;
    }

    std::printf("frames: trivial=%zu small=%zu large=%zu\n", trivial, small, large);
    std::printf("frames: overhead_only=%s tracks_live_locals=%s carries_4k_buffer=%s\n",
                trivial < 128 ? "yes" : "no", (small > trivial && small < large) ? "yes" : "no",
                large > 4096 ? "yes" : "no");
    std::printf("frames: a frame holds the promise, two function pointers, a state index, "
                "and the locals live across a suspend -- nothing else\n");
    report("frames");
}

// ── halo: the allocation that may not happen ─────────────────────────────
//
// HALO -- Heap Allocation eLision Optimization. When the compiler can prove a
// frame does not outlive its caller, [dcl.fct.def.coroutine] PERMITS it to
// elide the allocation entirely. Permits, not requires.
//
// This coroutine is the easy case: suspend_never at both ends, so it runs to
// completion inside the call and its frame provably dies there. If elision is
// going to happen anywhere, it happens here.
//
// Measured on this host, 1000 calls:
//
//   g++ 16.1.1    -O0 1000  -O1 1000  -O2 1000  -O3 1000   (never elides)
//   clang++ 22.1.8 -O0 1000  -O1 0     -O2 0               (elides everything)
//
// "The optimizer will remove it" is true on one of the two compilers here.
// Which is why this program counts rather than assumes.

struct Eager {
    struct promise_type {
        static void* operator new(std::size_t n) {
            ++g_alloc_count;
            return ::operator new(n);
        }
        static void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
        Eager get_return_object() { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
};

Eager eager_add(int* sink) {
    *sink += 1;
    co_return;
}

constexpr int kHaloCalls = 1000;

void case_halo() {
    int sink = 0;
    g_alloc_count = 0;
    for (int i = 0; i < kHaloCalls; ++i) {
        eager_add(&sink);
    }
    const std::size_t allocs = g_alloc_count;

    std::printf("halo: calls=%d allocations=%zu elided=%zu\n", kHaloCalls, allocs,
                static_cast<std::size_t>(kHaloCalls) - allocs);
    std::printf("halo: work_done=%d fully_elided=%s\n", sink, allocs == 0 ? "yes" : "no");
    std::printf("halo: elision is PERMITTED, never guaranteed -- measure it on the "
                "compiler you actually ship\n");
    report("halo");
}

// ── generator: what C++23 hands you ──────────────────────────────────────
//
// ch27 hand-rolled a generator: a promise_type with yield_value, a handle
// wrapper, and an iterator. C++23's <generator> is that, standardized, with
// the recursive-yield and allocator details handled.

std::generator<std::uint64_t> fibonacci() {
    std::uint64_t a = 0, b = 1;
    while (true) {
        co_yield a;
        const std::uint64_t next = a + b;
        a = b;
        b = next;
    }
}

void case_generator() {
    int taken = 0;
    std::uint64_t last = 0;
    for (const std::uint64_t v : fibonacci()) {
        if (++taken > 20) {
            break;
        }
        last = v;
    }

    // An infinite generator consumed finitely. Breaking out of the loop
    // destroys the generator, which destroys the frame -- the suspended
    // coroutine is simply abandoned, and that is well-defined.
    std::printf("generator: took=%d twentieth_fib=%llu expected=4181 correct=%s\n", taken - 1,
                static_cast<unsigned long long>(last), last == 4181 ? "yes" : "no");
    std::printf("generator: an infinite coroutine consumed finitely, then destroyed "
                "mid-suspension\n");
    report("generator");
}

// ── versus-thread: the number this chapter exists for ────────────────────

void case_versus_thread() {
    std::size_t frame = 0;
    {
        Counted c = frame_trivial();
        frame = g_last_frame;
    }

    const std::size_t ratio = frame > 0 ? kCh50ThreadStackBytes / frame : 0;

    std::printf("versus-thread: coroutine_frame=%zu bytes thread_stack=%zu bytes (ch50)\n", frame,
                kCh50ThreadStackBytes);
    std::printf("versus-thread: ratio=%zux frames_per_thread_stack=%zu\n", ratio, ratio);
    std::printf("versus-thread: three_orders_of_magnitude=%s\n", ratio >= 1000 ? "yes" : "no");
    std::printf("versus-thread: both hold one paused computation; only one of them "
                "reserves 8 MiB of address space to do it\n");
    report("versus-thread");
}

// ── lifetime: the trap ───────────────────────────────────────────────────
//
// A coroutine_handle is a raw pointer with a nicer name. It does not own the
// frame, it is not RAII, copying it copies a pointer, and destroying it does
// nothing at all. Whoever creates a coroutine must arrange for exactly one
// destroy() -- or the frame leaks, which is what an unowned handle does.
//
// This case demonstrates the leak DELIBERATELY and reports it, rather than
// pretending the problem does not exist. The Counted type above is the fix:
// a destructor that calls destroy().

void case_lifetime() {
    // The observable is not how many frames were ALLOCATED -- both halves
    // allocate one. It is how many were FREED. operator delete on the promise
    // runs only when someone calls destroy(), so counting it distinguishes a
    // frame that was cleaned up from one that was abandoned.
    g_alloc_count = g_free_count = 0;
    {
        auto leaked = frame_trivial();
        // Take the handle out of the RAII wrapper so its destructor cannot
        // clean up -- exactly what code that stores handles in a container
        // and loses track of one ends up doing.
        auto h = leaked.handle;
        leaked.handle = {};
        (void)h;  // never destroyed: this frame is now unreachable garbage
    }
    const std::size_t leaked_allocs = g_alloc_count;
    const std::size_t leaked_frees = g_free_count;

    // The same coroutine, owned properly.
    g_alloc_count = g_free_count = 0;
    {
        Counted owned = frame_trivial();
        // ~Counted() calls handle.destroy(), which runs operator delete
    }
    const std::size_t owned_allocs = g_alloc_count;
    const std::size_t owned_frees = g_free_count;

    std::printf("lifetime: abandoned alloc=%zu free=%zu leaked=%s\n", leaked_allocs, leaked_frees,
                leaked_frees < leaked_allocs ? "yes" : "no");
    std::printf("lifetime: owned     alloc=%zu free=%zu leaked=%s\n", owned_allocs, owned_frees,
                owned_frees < owned_allocs ? "yes" : "no");
    std::printf("lifetime: a coroutine_handle is a raw pointer -- it does not own the "
                "frame and destroying it frees nothing\n");
    std::printf("lifetime: exactly one destroy() per frame, and the type system will "
                "not remind you\n");
    report("lifetime");
}

void case_versions() {
    std::printf("versions: __cpp_impl_coroutine=%ld __cpp_lib_coroutine=%ld\n",
                static_cast<long>(__cpp_impl_coroutine), static_cast<long>(__cpp_lib_coroutine));
#ifdef __cpp_lib_generator
    std::printf("versions: __cpp_lib_generator=%ld (C++23 <generator> present)\n",
                static_cast<long>(__cpp_lib_generator));
#else
    std::printf("versions: __cpp_lib_generator=undefined\n");
#endif
#if defined(__clang__)
    std::printf("versions: compiler=clang %d.%d.%d\n", __clang_major__, __clang_minor__,
                __clang_patchlevel__);
#elif defined(__GNUC__)
    std::printf("versions: compiler=gcc %d.%d.%d\n", __GNUC__, __GNUC_MINOR__,
                __GNUC_PATCHLEVEL__);
#endif
    report("versions");
}

void usage() {
    std::fprintf(stderr, "usage: coro <versions|frames|halo|generator|versus-thread|lifetime>\n");
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
    } else if (what == "frames") {
        case_frames();
    } else if (what == "halo") {
        case_halo();
    } else if (what == "generator") {
        case_generator();
    } else if (what == "versus-thread") {
        case_versus_thread();
    } else if (what == "lifetime") {
        case_lifetime();
    } else {
        usage();
        return 2;
    }
    return 0;
}
