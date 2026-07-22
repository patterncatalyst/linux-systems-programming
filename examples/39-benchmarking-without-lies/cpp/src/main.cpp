// benchlab (C++23) — proper benchmarking of the chatterd frame codec (ch21).
//
// Usage:
//   app --op encode|decode|roundtrip --iters N [--warmup W]   # the real thing
//   app --lie [--op encode|decode|roundtrip]                  # the anti-pattern
//
// The "real thing" times each iteration individually with a monotonic clock,
// discards a warmup phase, and reports min/median/p99/max plus a
// coordinated-omission-corrected p99 (see co_correct below). `--lie` times a
// single unwarmed call with wall-clock and reports one number — exactly the
// benchmark the rest of this example exists to discredit.
//
// Wire format (canonical chatterd chat frame, introduced ch21):
//   [ magic 0x43 0x48 ][ version 0x01 ][ type u8 ][ length u16 be ][ payload ]
// This file re-implements only encode_frame/decode_frame — the codec under
// test — not the daemon; see ch21/22/27 for the networked version.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Frame codec under test — canonical chatterd chat frame (see file header).
// ---------------------------------------------------------------------------
constexpr std::size_t kHeaderLen = 6;
constexpr std::uint8_t kMagic0 = 0x43;
constexpr std::uint8_t kMagic1 = 0x48;
constexpr std::uint8_t kVersion = 0x01;
constexpr std::uint8_t kTypeDeliver = 3;

struct DecodedFrame {
    std::uint8_t type;
    std::string body;
};

std::string encode_frame(std::uint8_t type, std::string_view body) {
    std::string out;
    out.reserve(kHeaderLen + body.size());
    out.push_back(static_cast<char>(kMagic0));
    out.push_back(static_cast<char>(kMagic1));
    out.push_back(static_cast<char>(kVersion));
    out.push_back(static_cast<char>(type));
    auto len = static_cast<std::uint16_t>(body.size());
    out.push_back(static_cast<char>((len >> 8) & 0xff));
    out.push_back(static_cast<char>(len & 0xff));
    out.append(body);
    return out;
}

// Decodes exactly one frame that fills `buf` completely — no partial-read
// reassembly here, that lives in the networked chatterd (ch21/22/27); this
// harness only measures the codec's per-call cost.
[[nodiscard]] std::expected<DecodedFrame, std::string> decode_frame(std::string_view buf) {
    if (buf.size() < kHeaderLen) {
        return std::unexpected("frame shorter than header");
    }
    if (static_cast<std::uint8_t>(buf[0]) != kMagic0 || static_cast<std::uint8_t>(buf[1]) != kMagic1) {
        return std::unexpected("bad magic");
    }
    if (static_cast<std::uint8_t>(buf[2]) != kVersion) {
        return std::unexpected("bad version");
    }
    std::uint8_t type = static_cast<std::uint8_t>(buf[3]);
    std::uint16_t len = static_cast<std::uint16_t>((static_cast<std::uint8_t>(buf[4]) << 8) |
                                                    static_cast<std::uint8_t>(buf[5]));
    if (buf.size() != kHeaderLen + len) {
        return std::unexpected("length mismatch");
    }
    return DecodedFrame{type, std::string(buf.substr(kHeaderLen))};
}

// ---------------------------------------------------------------------------
// The fixed workload: a DELIVER frame carrying a realistic chat line. Every
// language in this example encodes/decodes the identical bytes.
// ---------------------------------------------------------------------------
constexpr std::string_view kNick = "alice";
constexpr std::string_view kText = "the quick brown fox jumps over the lazy dog, three times, for benchlab";

std::string delivery_body() {
    std::string body;
    body.reserve(kNick.size() + 1 + kText.size());
    body.append(kNick);
    body.push_back('\0');
    body.append(kText);
    return body;
}

enum class Op { Encode, Decode, Roundtrip };

std::expected<Op, std::string> parse_op(std::string_view s) {
    if (s == "encode") return Op::Encode;
    if (s == "decode") return Op::Decode;
    if (s == "roundtrip") return Op::Roundtrip;
    return std::unexpected("unknown --op '" + std::string(s) + "'");
}

std::string_view op_name(Op op) {
    switch (op) {
        case Op::Encode: return "encode";
        case Op::Decode: return "decode";
        case Op::Roundtrip: return "roundtrip";
    }
    return "?";
}

// One iteration of the requested op against the fixed workload. Returns a
// small scalar derived from the result so the caller can fold it into a
// checksum — this is what keeps the optimizer from proving the call's result
// is unused and deleting it.
std::uint64_t run_once(Op op, const std::string& body, const std::string& prebuilt_frame) {
    switch (op) {
        case Op::Encode: {
            std::string frame = encode_frame(kTypeDeliver, body);
            return frame.empty() ? 0 : static_cast<std::uint8_t>(frame.back());
        }
        case Op::Decode: {
            auto decoded = decode_frame(prebuilt_frame);
            if (!decoded) {
                std::println(stderr, "benchlab: codec bug: decode failed: {}", decoded.error());
                std::exit(1);
            }
            return decoded->body.size();
        }
        case Op::Roundtrip: {
            std::string frame = encode_frame(kTypeDeliver, body);
            auto decoded = decode_frame(frame);
            if (!decoded) {
                std::println(stderr, "benchlab: codec bug: decode failed: {}", decoded.error());
                std::exit(1);
            }
            return decoded->body.size();
        }
    }
    return 0;
}

using Clock = std::chrono::steady_clock;

std::int64_t ns_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

std::int64_t percentile(const std::vector<std::int64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

// Coordinated-omission correction (HdrHistogram's recordValueWithExpectedInterval):
// this harness is a closed loop — iteration N+1 never starts until N returns —
// so a stall (a page fault, a context switch, a scheduling hiccup) is
// recorded as a single slow sample. A fixed-rate caller would instead have
// had many requests queue up behind that stall, each experiencing a similar
// multiple-of-the-target-interval delay. This backfills those missing
// virtual samples so the tail reflects what a real caller would have seen,
// not just what one lucky/unlucky iteration measured.
std::vector<std::int64_t> co_correct(const std::vector<std::int64_t>& raw,
                                      std::int64_t expected_interval_ns,
                                      std::size_t cap) {
    std::vector<std::int64_t> out;
    out.reserve(raw.size());
    for (std::int64_t v : raw) {
        out.push_back(v);
        if (expected_interval_ns <= 0 || v <= expected_interval_ns) continue;
        std::int64_t missing = v - expected_interval_ns;
        while (missing >= expected_interval_ns && out.size() < cap) {
            out.push_back(missing);
            missing -= expected_interval_ns;
        }
        if (out.size() >= cap) break;
    }
    return out;
}

void usage_and_exit() {
    std::println(stderr, "usage: benchlab --op encode|decode|roundtrip --iters N [--warmup W]");
    std::println(stderr, "       benchlab --lie [--op encode|decode|roundtrip]");
    std::exit(2);
}

std::expected<long long, std::string> parse_u64(std::string_view s) {
    if (s.empty()) return std::unexpected("empty number");
    long long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::unexpected("not a number: " + std::string(s));
        v = v * 10 + (c - '0');
    }
    return v;
}

} // namespace

int main(int argc, char** argv) {
    bool lie = false;
    std::string op_str;
    long long iters = -1;
    long long warmup = 1000;

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--lie") {
            lie = true;
        } else if (a == "--op" && i + 1 < argc) {
            op_str = argv[++i];
        } else if (a == "--iters" && i + 1 < argc) {
            auto v = parse_u64(argv[++i]);
            if (!v) usage_and_exit();
            iters = *v;
        } else if (a == "--warmup" && i + 1 < argc) {
            auto v = parse_u64(argv[++i]);
            if (!v) usage_and_exit();
            warmup = *v;
        } else {
            usage_and_exit();
        }
    }

    if (!lie && op_str.empty()) usage_and_exit();
    Op op = Op::Roundtrip;
    if (!op_str.empty()) {
        auto parsed = parse_op(op_str);
        if (!parsed) usage_and_exit();
        op = *parsed;
    }

    const std::string body = delivery_body();
    const std::string prebuilt = encode_frame(kTypeDeliver, body);

    if (lie) {
        // The anti-pattern: no warmup, one call, wall-clock, done. This is
        // exactly the benchmark this example's README argues never to trust.
        auto t0 = Clock::now();
        std::uint64_t sink = run_once(op, body, prebuilt);
        auto t1 = Clock::now();
        std::println("benchlab: lie op={} (no warmup, single wall-clock sample, ignores variance)", op_name(op));
        std::println("benchlab: lie elapsed_ns={} sink={}", ns_between(t0, t1), sink);
        return 0;
    }

    if (iters < 2 || warmup < 0 || iters <= warmup) usage_and_exit();

    // Warmup: run the op without timing it, letting allocators/caches/branch
    // predictors reach steady state before any sample is recorded.
    std::uint64_t checksum = 0;
    for (long long i = 0; i < warmup; ++i) {
        checksum += run_once(op, body, prebuilt);
    }

    std::vector<std::int64_t> raw;
    raw.reserve(static_cast<std::size_t>(iters));
    for (long long i = 0; i < iters; ++i) {
        auto t0 = Clock::now();
        checksum += run_once(op, body, prebuilt);
        auto t1 = Clock::now();
        raw.push_back(ns_between(t0, t1));
    }

    std::vector<std::int64_t> sorted = raw;
    std::sort(sorted.begin(), sorted.end());
    std::int64_t min_ns = sorted.front();
    std::int64_t median_ns = percentile(sorted, 0.5);
    std::int64_t p99_ns = percentile(sorted, 0.99);
    std::int64_t max_ns = sorted.back();

    std::int64_t expected_interval_ns = median_ns > 0 ? median_ns : 1;
    constexpr std::size_t kCoCap = 5'000'000;
    std::vector<std::int64_t> corrected = co_correct(raw, expected_interval_ns, kCoCap);
    std::sort(corrected.begin(), corrected.end());
    std::int64_t co_p99_ns = percentile(corrected, 0.99);

    std::println("benchlab: op={} iters={} warmup={}", op_name(op), iters, warmup);
    std::println("benchlab: n={} min_ns={} median_ns={} p99_ns={} max_ns={}",
                 sorted.size(), min_ns, median_ns, p99_ns, max_ns);
    std::println("benchlab: co_p99_ns={} expected_interval_ns={} co_n={}",
                 co_p99_ns, expected_interval_ns, corrected.size());
    std::println("benchlab: checksum={:016x}", checksum);
    return 0;
}
