// toolbelt — three focused profiling targets, one binary per language, each
// exercised by that language's native profiler (chapter 31: per-language
// toolbelts).
//
//   app <hot|alloc> [--n N]
//
// hot:   counts primes in [2, n) by trial division. spin_hot is the only
//        function that does real work — it dominates a CPU profile.
// alloc: builds a 1000-key string index by round-robin overwrite from n
//        iterations of freshly allocated strings. alloc_churn is the only
//        allocation site — it dominates an allocation profile.
//
// Output: "app: mode=<hot|alloc> n=<n> result=<r> ms=<t>" on success.
// spin_hot has C linkage on purpose: perf/gdb show it as the plain symbol
// "spin_hot" instead of an Itanium-mangled name, which keeps the profiler
// output (and the pattern that greps it) simple.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

constexpr long kDefaultHotN = 3'000'000;
constexpr long kDefaultAllocN = 200'000;
constexpr long kKeyspace = 1'000; // distinct keys; the rest is churn

struct Config {
    std::string mode;
    long n = 0;
};

// RAII scoped timer: measures its own lifetime and writes the elapsed
// milliseconds into `out_ms` on destruction, however the scope is left.
class ScopedTimer {
  public:
    explicit ScopedTimer(long& out_ms) noexcept
        : out_ms_(out_ms), start_(std::chrono::steady_clock::now()) {}
    ~ScopedTimer() {
        const auto end = std::chrono::steady_clock::now();
        out_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_).count();
    }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

  private:
    long& out_ms_;
    std::chrono::steady_clock::time_point start_;
};

[[nodiscard]] std::expected<long, std::string> parse_positive(std::string_view s) {
    long v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size() || v <= 0) {
        return std::unexpected("not a positive integer: " + std::string{s});
    }
    return v;
}

[[nodiscard]] std::expected<Config, std::string> parse_args(int argc, char** argv) {
    if (argc < 2) {
        return std::unexpected("missing mode");
    }
    Config cfg;
    cfg.mode = argv[1];
    if (cfg.mode == "hot") {
        cfg.n = kDefaultHotN;
    } else if (cfg.mode == "alloc") {
        cfg.n = kDefaultAllocN;
    } else {
        return std::unexpected("unknown mode: " + cfg.mode);
    }

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--n" && i + 1 < argc) {
            const auto v = parse_positive(argv[++i]);
            if (!v) {
                return std::unexpected(v.error());
            }
            cfg.n = *v;
        } else {
            return std::unexpected("unknown argument: " + std::string{arg});
        }
    }
    return cfg;
}

// The CPU-bound target: trial division is deliberately unoptimized (checks
// every candidate divisor up to sqrt(i)) so a couple of seconds of runtime
// buys a profile with a single, unmistakable hot frame.
extern "C" [[gnu::noinline]] std::uint64_t spin_hot(long n) {
    std::uint64_t count = 0;
    for (long i = 2; i < n; ++i) {
        bool prime = true;
        for (long d = 2; d * d <= i; ++d) {
            if (i % d == 0) {
                prime = false;
                break;
            }
        }
        if (prime) {
            ++count;
        }
    }
    return count;
}

// The allocation-heavy target: n iterations of churn, each a fresh key and
// value string, round-robin overwriting a 1000-entry index. Every
// allocation in the program happens inside this one function.
[[nodiscard]] std::expected<std::uint64_t, std::string> alloc_churn(long n) {
    std::unordered_map<std::string, std::string> index;
    index.reserve(kKeyspace);
    for (long i = 0; i < n; ++i) {
        const long idx = i % kKeyspace;
        index.insert_or_assign(std::format("k{}", idx), std::format("{:x}", i));
    }

    const long want = std::min(n, kKeyspace);
    if (index.size() != static_cast<std::size_t>(want)) {
        return std::unexpected(std::format("index has {} entries, want {}", index.size(), want));
    }
    std::uint64_t total = 0;
    for (const auto& [k, v] : index) {
        total += v.size();
    }
    if (total == 0) {
        return std::unexpected("summed zero bytes");
    }
    return total;
}

} // namespace

int main(int argc, char** argv) {
    const auto cfg = parse_args(argc, argv);
    if (!cfg) {
        std::println(stderr, "app: {}", cfg.error());
        std::println(stderr, "usage: app <hot|alloc> [--n N]");
        return 2;
    }

    long ms = 0;
    std::expected<std::uint64_t, std::string> result;
    {
        ScopedTimer timer(ms);
        result = (cfg->mode == "hot") ? std::expected<std::uint64_t, std::string>(spin_hot(cfg->n))
                                      : alloc_churn(cfg->n);
    }
    if (!result) {
        std::println(stderr, "app: {}", result.error());
        return 1;
    }

    std::println("app: mode={} n={} result={} ms={}", cfg->mode, cfg->n, *result, ms);
    return 0;
}
