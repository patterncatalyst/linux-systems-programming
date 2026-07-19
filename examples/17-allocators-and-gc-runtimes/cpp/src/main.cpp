// allocbench — allocation-heavy index build/query (chapter 17: allocators
// and GC runtimes).
//
//   allocbench [--allocs N] [--variant default|arena]
//
// Both variants build the same string->string index (1000 distinct keys,
// overwritten round-robin) from N iterations of short-lived intermediate
// strings, then query every distinct key back. What differs is where the
// intermediates live:
//
//   default: every key/frag/value is a plain std::string — each a trip to
//            the global heap (malloc), each freed individually when its
//            batch scratch vector is destroyed.
//   arena:   per batch of 1000 iterations, a std::pmr::monotonic_buffer_-
//            resource carves the intermediates (std::pmr::string in a
//            std::pmr::vector) out of one reusable slab. Nothing is freed
//            per-object; the whole batch is released at once when the
//            resource is destroyed, and the slab is reused for the next
//            batch. Only the final key/value pairs are copied out into the
//            long-lived index.
//
// Reports: "allocbench: variant=<v> allocs=<n> peak_rss=<kb>KB ms=<t>"
// (peak_rss is getrusage(2) ru_maxrss — the VmHWM high-water mark — in KB).

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory_resource>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/resource.h>

namespace {

using std::expected;
using std::unexpected;

constexpr long kDefaultAllocs = 200'000;
constexpr long kKeyspace = 1'000;  // distinct keys; the rest is churn
constexpr long kBatch = 1'000;     // arena reset granularity (iterations)
constexpr int kRepeat = 4;         // value = frag repeated kRepeat times
constexpr std::size_t kSlabBytes = 256 * 1024;  // reusable arena slab

using Index = std::unordered_map<std::string, std::string>;

struct Config {
    long allocs = kDefaultAllocs;
    bool arena = false;
};

[[nodiscard]] std::error_code errno_ec() {
    return std::error_code{errno, std::system_category()};
}

[[nodiscard]] expected<long, std::string> parse_positive(std::string_view s) {
    long v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size() || v <= 0) {
        return unexpected("not a positive integer: " + std::string{s});
    }
    return v;
}

[[nodiscard]] expected<Config, std::string> parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--allocs" && i + 1 < argc) {
            const auto v = parse_positive(argv[++i]);
            if (!v) {
                return unexpected(v.error());
            }
            cfg.allocs = *v;
        } else if (arg == "--variant" && i + 1 < argc) {
            const std::string_view v = argv[++i];
            if (v == "default") {
                cfg.arena = false;
            } else if (v == "arena") {
                cfg.arena = true;
            } else {
                return unexpected("unknown variant: " + std::string{v});
            }
        } else {
            return unexpected("unknown argument: " + std::string{arg});
        }
    }
    return cfg;
}

// Append the decimal form of v without allocating (unlike std::to_string,
// which would hand the arena variant a heap std::string behind our back).
template <typename Str>
void append_num(Str& out, long v) {
    std::array<char, 20> buf;
    const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    out.append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
}

// One iteration of churn: key "key-<idx>", frag "value-<idx>-<i%97>/",
// value = frag x kRepeat. Works on std::string and std::pmr::string alike.
template <typename Str>
void make_kv(long i, Str& key, Str& frag, Str& value) {
    const long idx = i % kKeyspace;
    key.append("key-", 4);
    append_num(key, idx);
    frag.append("value-", 6);
    append_num(frag, idx);
    frag.push_back('-');
    append_num(frag, i % 97);
    frag.push_back('/');
    for (int r = 0; r < kRepeat; ++r) {
        value.append(frag.data(), frag.size());
    }
}

// default variant: plain heap strings, freed one by one at batch end.
[[nodiscard]] Index build_default(long allocs) {
    Index index;
    index.reserve(kKeyspace);
    for (long start = 0; start < allocs; start += kBatch) {
        const long end = std::min(start + kBatch, allocs);
        std::vector<std::string> scratch;  // keeps the churn alive per batch
        scratch.reserve(static_cast<std::size_t>(end - start));
        for (long i = start; i < end; ++i) {
            std::string key;
            std::string frag;
            std::string value;
            make_kv(i, key, frag, value);
            index.insert_or_assign(std::move(key), std::move(value));
            scratch.push_back(std::move(frag));
        }
    }  // ~scratch: one deallocation per surviving string
    return index;
}

// arena variant: same shape, but the intermediates come from a monotonic
// buffer over one reusable slab and are released wholesale per batch.
[[nodiscard]] Index build_arena(long allocs) {
    std::vector<std::byte> slab(kSlabBytes);  // reused by every batch
    Index index;
    index.reserve(kKeyspace);
    for (long start = 0; start < allocs; start += kBatch) {
        const long end = std::min(start + kBatch, allocs);
        std::pmr::monotonic_buffer_resource arena{slab.data(), slab.size()};
        std::pmr::vector<std::pmr::string> scratch{&arena};
        scratch.reserve(static_cast<std::size_t>(end - start));
        for (long i = start; i < end; ++i) {
            std::pmr::string key{&arena};
            std::pmr::string frag{&arena};
            std::pmr::string value{&arena};
            make_kv(i, key, frag, value);
            index.insert_or_assign(std::string{key}, std::string{value});
            scratch.push_back(std::move(frag));
        }
    }  // ~arena: the whole batch vanishes at once; no per-object frees
    return index;
}

[[nodiscard]] expected<std::uint64_t, std::string> query(const Index& index, long allocs) {
    const long distinct = std::min(allocs, kKeyspace);
    std::uint64_t total = 0;
    for (long idx = 0; idx < distinct; ++idx) {
        std::string key{"key-"};
        append_num(key, idx);
        const auto it = index.find(key);
        if (it == index.end()) {
            return unexpected("missing key: " + key);
        }
        total += it->second.size();
    }
    if (total == 0) {
        return unexpected("query summed zero bytes");
    }
    return total;
}

[[nodiscard]] expected<long, std::error_code> peak_rss_kb() {
    rusage ru{};
    if (::getrusage(RUSAGE_SELF, &ru) != 0) {
        return unexpected(errno_ec());
    }
    return ru.ru_maxrss;  // kilobytes on Linux
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = parse_args(argc, argv);
    if (!cfg) {
        std::println(stderr, "allocbench: {}", cfg.error());
        std::println(stderr, "usage: allocbench [--allocs N] [--variant default|arena]");
        return 2;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const Index index = cfg->arena ? build_arena(cfg->allocs) : build_default(cfg->allocs);
    const auto total = query(index, cfg->allocs);
    const auto t1 = std::chrono::steady_clock::now();
    if (!total) {
        std::println(stderr, "allocbench: {}", total.error());
        return 1;
    }

    const auto rss = peak_rss_kb();
    if (!rss) {
        std::println(stderr, "allocbench: getrusage: {}", rss.error().message());
        return 1;
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::println("allocbench: variant={} allocs={} peak_rss={}KB ms={}",
                 cfg->arena ? "arena" : "default", cfg->allocs, *rss, ms);
    return 0;
}
