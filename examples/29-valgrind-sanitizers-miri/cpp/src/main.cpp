// bugfarm — chapter 29: valgrind, sanitizers, and miri.
//
//   app <leak|uaf|uninit|overflow|race>
//
// Each subcommand runs a tiny routine exhibiting exactly one seeded memory
// or concurrency defect. C++ gives no compile-time protection against any
// of the five, so this binary expresses all of them; run it under the
// matching tool to see each one caught:
//
//   leak      -> valgrind --leak-check=full   ("definitely lost")
//   uninit    -> valgrind                     ("uninitialised value(s)")
//   uaf       -> ASan build                   ("heap-use-after-free")
//   overflow  -> ASan build                   ("heap-buffer-overflow")
//   race      -> TSan build                   ("data race")
//
// Plain (uninstrumented) runs are deliberately left un-hardened: leak and
// uninit stay silent, uaf usually "works" by accident, race just produces
// a wrong count -- and overflow can abort with a glibc heap-corruption
// message when its write clobbers the next chunk's bookkeeping. That
// unpredictability *is* the lesson: these bugs hide from a normal run and
// need a tool that watches memory, not eyeballs on stdout.

#include <cstddef>
#include <cstdio>
#include <expected>
#include <print>
#include <string_view>
#include <thread>
#include <utility>

namespace {

enum class Bug { leak, uaf, uninit, overflow, race };

[[nodiscard]] std::expected<Bug, std::string_view> parse_bug(std::string_view arg) {
    if (arg == "leak") {
        return Bug::leak;
    }
    if (arg == "uaf") {
        return Bug::uaf;
    }
    if (arg == "uninit") {
        return Bug::uninit;
    }
    if (arg == "overflow") {
        return Bug::overflow;
    }
    if (arg == "race") {
        return Bug::race;
    }
    return std::unexpected(arg);
}

// Defeats the optimizer's ability to prove an allocation is never
// otherwise used and fold the whole thing away. Observed on this host at
// -O2 without this barrier: an unused `malloc`/`new` (and the memory
// pattern written into it) is deleted outright by GCC, taking the "bug"
// with it before valgrind ever sees an allocation. Same trick as
// benchmark libraries' DoNotOptimize.
template <class T> inline void touch(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

int counter = 0; // race: shared, unsynchronized on purpose

void bump() {
    for (int i = 0; i < 200'000; ++i) {
        ++counter;
    }
}

int do_leak() {
    int* buf = new int[1024];
    for (int i = 0; i < 1024; ++i) {
        buf[i] = i;
    }
    touch(buf); // keep the allocation "used"; never delete[] it
    std::println("bugfarm: leak: allocated {} bytes on the heap, never freed (intentional)",
                 1024 * sizeof(int));
    return 0;
}

int do_uaf() {
    int* p = new int(42);
    delete p;
    touch(p);
    *p = 7; // heap-use-after-free: write through a dangling pointer
    std::println("bugfarm: uaf: wrote through a freed pointer, read back {}", *p);
    return 0;
}

int do_uninit() {
    int* p = new int; // `new int` with no initializer: value is indeterminate
    touch(p);
    if (*p == 0x1234) {
        std::println("bugfarm: uninit: buf had 0x1234 (unlikely)");
    } else {
        std::println("bugfarm: uninit: read of an uninitialised int: {}", *p);
    }
    delete p;
    return 0;
}

int do_overflow() {
    int* arr = new int[10];
    for (int i = 0; i < 10; ++i) {
        arr[i] = i;
    }
    touch(arr);
    arr[10] = 99; // heap-buffer-overflow: one past the allocated end
    std::println("bugfarm: overflow: wrote arr[10]={} (one past the allocated end)", arr[10]);
    delete[] arr;
    return 0;
}

int do_race() {
    {
        std::jthread t1(bump); // RAII joins both on scope exit -- lifetime
        std::jthread t2(bump); // is safe; the *data* race on `counter` is not
    }
    std::println("bugfarm: race: counter={} (expected 400000; a wrong value is the benign symptom)",
                 counter);
    return 0;
}

constexpr std::string_view kUsage = "usage: app <leak|uaf|uninit|overflow|race>";

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::println(stderr, "{}", kUsage);
        return 2;
    }
    const auto bug = parse_bug(argv[1]);
    if (!bug) {
        std::println(stderr, "app: unknown bug: {}\n{}", bug.error(), kUsage);
        return 2;
    }
    switch (*bug) {
    case Bug::leak:
        return do_leak();
    case Bug::uaf:
        return do_uaf();
    case Bug::uninit:
        return do_uninit();
    case Bug::overflow:
        return do_overflow();
    case Bug::race:
        return do_race();
    }
    std::unreachable();
}
