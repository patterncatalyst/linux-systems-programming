// spscring: a single-producer/single-consumer lock-free ring buffer.
//
//   spscring --capacity K --items N [--pad on|off]
//
// A producer thread pushes N u64 values (0, 1, ..., N-1) through a bounded
// ring of K slots; the consumer (this thread) pops and sums them. The head
// and tail indices are std::atomic<uint64_t> monotonic counters synchronised
// with explicit acquire/release ordering (Lamport's SPSC queue) — no mutex,
// no lock. --pad places head and tail on separate cache lines to remove the
// false sharing that otherwise ping-pongs the line between the two cores.
//
// Prints exactly one line:
//   spscring: items=<N> sum=<s> throughput_mops=<m> pad=<on|off>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <print>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view kUsage =
    "usage: spscring --capacity K --items N [--pad on|off]";

struct Args {
    std::uint64_t capacity = 0;
    std::uint64_t items = 0;
    bool pad = false;
};

[[nodiscard]] std::optional<std::uint64_t> parse_u64(std::string_view s) {
    std::uint64_t v = 0;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return v;
}

// Parse argv into Args. Returns nullopt on any malformed input (caller then
// prints the usage line and exits 2).
[[nodiscard]] std::optional<Args> parse_args(int argc, char** argv) {
    Args a{};
    bool have_cap = false;
    bool have_items = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        auto next_value = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= argc) {
                return std::nullopt;
            }
            return std::string_view{argv[++i]};
        };
        if (flag == "--capacity") {
            const auto v = next_value();
            if (!v) return std::nullopt;
            const auto n = parse_u64(*v);
            if (!n || *n == 0) return std::nullopt;
            a.capacity = *n;
            have_cap = true;
        } else if (flag == "--items") {
            const auto v = next_value();
            if (!v) return std::nullopt;
            const auto n = parse_u64(*v);
            if (!n) return std::nullopt;
            a.items = *n;
            have_items = true;
        } else if (flag == "--pad") {
            const auto v = next_value();
            if (!v) return std::nullopt;
            if (*v == "on") {
                a.pad = true;
            } else if (*v == "off") {
                a.pad = false;
            } else {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    }
    if (!have_cap || !have_items) {
        return std::nullopt;
    }
    return a;
}

// Control block: the two index atomics. The padded specialisation forces head
// and tail onto separate 64-byte cache lines; the packed one leaves them
// adjacent so the two cores fight over one line (false sharing).
template <bool Pad>
struct Control;

template <>
struct Control<true> {
    alignas(64) std::atomic<std::uint64_t> head{0};
    alignas(64) std::atomic<std::uint64_t> tail{0};
};

template <>
struct Control<false> {
    std::atomic<std::uint64_t> head{0};
    std::atomic<std::uint64_t> tail{0};
};

struct Result {
    std::uint64_t sum;
    double mops;
};

template <bool Pad>
[[nodiscard]] Result run_bench(std::uint64_t cap, std::uint64_t items) {
    std::vector<std::uint64_t> buf(cap);
    auto ctrl = std::make_unique<Control<Pad>>();
    auto& head = ctrl->head;
    auto& tail = ctrl->tail;

    const auto start = std::chrono::steady_clock::now();

    // Producer: push 0..items-1. jthread joins on scope exit.
    std::jthread producer([&] {
        std::uint64_t t = 0;
        for (std::uint64_t i = 0; i < items; ++i) {
            // Wait until the ring has a free slot (consumer advanced head).
            while (t - head.load(std::memory_order_acquire) == cap) {
                // spin
            }
            buf[t % cap] = i;
            ++t;
            tail.store(t, std::memory_order_release);
        }
    });

    // Consumer runs on this thread.
    std::uint64_t sum = 0;
    std::uint64_t h = 0;
    for (std::uint64_t i = 0; i < items; ++i) {
        while (h == tail.load(std::memory_order_acquire)) {
            // spin: ring empty
        }
        sum += buf[h % cap];
        ++h;
        head.store(h, std::memory_order_release);
    }

    producer.join();
    const auto end = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(end - start).count();
    if (secs < 1e-9) {
        secs = 1e-9;
    }
    const double mops = static_cast<double>(items) / secs / 1e6;
    return {sum, mops};
}

} // namespace

int main(int argc, char** argv) {
    const auto args = parse_args(argc, argv);
    if (!args) {
        std::println(stderr, "{}", kUsage);
        return 2;
    }

    const Result r = args->pad ? run_bench<true>(args->capacity, args->items)
                               : run_bench<false>(args->capacity, args->items);

    std::println("spscring: items={} sum={} throughput_mops={:.2f} pad={}",
                 args->items, r.sum, r.mops, args->pad ? "on" : "off");
    return 0;
}
