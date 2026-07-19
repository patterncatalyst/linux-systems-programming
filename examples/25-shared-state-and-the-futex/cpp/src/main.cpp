// workq — a bounded MPMC job queue and worker pool (C++23).
//
// P producers push N items total into a bounded blocking queue; C consumers
// pop items and fold each item's payload into a running checksum. The correct
// path synchronizes with std::mutex + std::condition_variable (the futex-backed
// primitives this chapter is about); --buggy swaps the per-consumer local
// accumulators for a single UNSYNCHRONIZED shared counter + checksum so
// ThreadSanitizer (build/tsan) has a real data race to report.
//
// The item set {0..N-1} is fixed for a given (seed, N), each item's payload is
// a pure function of its index, and the checksum folds them with XOR — which is
// commutative — so the correct run's checksum is deterministic regardless of P,
// C, or the order consumers happen to drain the queue.

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <expected>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ull;
constexpr std::uint64_t kMix1 = 0xBF58476D1CE4E5B9ull;
constexpr std::uint64_t kMix2 = 0x94D049BB133111EBull;
constexpr std::uint64_t kSeedDefault = 0x0123456789ABCDEFull;

// Pure per-index payload: a splitmix64 finalizer over seed + (i+1)*golden.
// uint64_t arithmetic wraps by definition, so this is identical in Go and Rust.
std::uint64_t Payload(std::uint64_t seed, std::uint64_t i) {
  std::uint64_t x = seed + (i + 1) * kGolden;
  x = (x ^ (x >> 30)) * kMix1;
  x = (x ^ (x >> 27)) * kMix2;
  x = x ^ (x >> 31);
  return x;
}

struct Config {
  long producers = 0;
  long consumers = 0;
  long items = 0;  // N, the total across all producers
  std::uint64_t seed = kSeedDefault;
  long cap = 256;
  bool buggy = false;
};

struct ParseError {
  std::string message;  // printed after the "workq: " prefix; empty => usage only
};

constexpr std::string_view kUsage =
    "usage: workq --producers P --consumers C --items N [--buggy] "
    "[--seed S] [--cap K]";

std::expected<long, std::string> ParseLong(std::string_view s) {
  long v = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || ptr != s.data() + s.size()) {
    return std::unexpected(std::string("not an integer: ") + std::string(s));
  }
  return v;
}

std::expected<std::uint64_t, std::string> ParseSeed(std::string_view s) {
  int base = 10;
  std::string_view body = s;
  if (s.size() > 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X")) {
    base = 16;
    body = s.substr(2);
  }
  std::uint64_t v = 0;
  auto [ptr, ec] = std::from_chars(body.data(), body.data() + body.size(), v, base);
  if (ec != std::errc{} || ptr != body.data() + body.size()) {
    return std::unexpected(std::string("not an integer: ") + std::string(s));
  }
  return v;
}

std::expected<Config, ParseError> Parse(int argc, char** argv) {
  Config c;
  bool has_p = false, has_c = false, has_n = false;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto value = [&](std::string_view* out) -> bool {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    if (a == "--buggy") {
      c.buggy = true;
    } else if (a == "--producers" || a == "--consumers" || a == "--items" ||
               a == "--cap") {
      std::string_view v;
      if (!value(&v)) return std::unexpected(ParseError{std::string(a) + " needs a value"});
      auto n = ParseLong(v);
      if (!n) return std::unexpected(ParseError{n.error()});
      if (a == "--producers") { c.producers = *n; has_p = true; }
      else if (a == "--consumers") { c.consumers = *n; has_c = true; }
      else if (a == "--items") { c.items = *n; has_n = true; }
      else { c.cap = *n; }
    } else if (a == "--seed") {
      std::string_view v;
      if (!value(&v)) return std::unexpected(ParseError{std::string(a) + " needs a value"});
      auto s = ParseSeed(v);
      if (!s) return std::unexpected(ParseError{s.error()});
      c.seed = *s;
    } else {
      return std::unexpected(ParseError{"unknown flag: " + std::string(a)});
    }
  }
  if (!has_p || !has_c || !has_n) {
    return std::unexpected(ParseError{""});  // usage only
  }
  if (c.producers < 1 || c.consumers < 1) {
    return std::unexpected(ParseError{"--producers and --consumers must be >= 1"});
  }
  if (c.items < 0) {
    return std::unexpected(ParseError{"--items must be >= 0"});
  }
  if (c.cap < 1) {
    return std::unexpected(ParseError{"--cap must be >= 1"});
  }
  return c;
}

// A bounded blocking MPMC queue: one mutex, two condition variables. Producers
// block on not_full_, consumers block on not_empty_; Close() wakes drained
// consumers. This is the shared state whose access the futex serializes.
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t cap) : cap_(cap) {}

  void Push(std::uint64_t v) {
    std::unique_lock lock(mu_);
    not_full_.wait(lock, [&] { return q_.size() < cap_; });
    q_.push_back(v);
    not_empty_.notify_one();
  }

  // Pop one item; returns false once the queue is closed and drained.
  bool Pop(std::uint64_t& out) {
    std::unique_lock lock(mu_);
    not_empty_.wait(lock, [&] { return !q_.empty() || closed_; });
    if (q_.empty()) return false;  // closed_ is true here
    out = q_.front();
    q_.pop_front();
    not_full_.notify_one();
    return true;
  }

  void Close() {
    {
      std::unique_lock lock(mu_);
      closed_ = true;
    }
    not_empty_.notify_all();
  }

 private:
  std::mutex mu_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::deque<std::uint64_t> q_;
  std::size_t cap_;
  bool closed_ = false;
};

int Run(const Config& cfg) {
  BoundedQueue queue(static_cast<std::size_t>(cfg.cap));
  std::atomic<long> produced{0};

  // Correct path: each consumer folds into its own slot; no shared writes.
  std::vector<long> local_counts(static_cast<std::size_t>(cfg.consumers), 0);
  std::vector<std::uint64_t> local_sums(static_cast<std::size_t>(cfg.consumers), 0);

  // Buggy path: one shared counter + checksum, mutated with no lock at all.
  long shared_consumed = 0;
  std::uint64_t shared_checksum = 0;

  const auto t0 = std::chrono::steady_clock::now();

  std::vector<std::jthread> producers;
  producers.reserve(static_cast<std::size_t>(cfg.producers));
  for (long p = 0; p < cfg.producers; ++p) {
    producers.emplace_back([&, p] {
      for (long i = p; i < cfg.items; i += cfg.producers) {
        queue.Push(Payload(cfg.seed, static_cast<std::uint64_t>(i)));
        produced.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::vector<std::jthread> consumers;
  consumers.reserve(static_cast<std::size_t>(cfg.consumers));
  for (long idx = 0; idx < cfg.consumers; ++idx) {
    consumers.emplace_back([&, idx] {
      std::uint64_t v;
      while (queue.Pop(v)) {
        if (cfg.buggy) {
          shared_consumed += 1;    // DATA RACE: unsynchronized shared counter
          shared_checksum ^= v;    // DATA RACE: unsynchronized shared checksum
        } else {
          local_counts[static_cast<std::size_t>(idx)] += 1;
          local_sums[static_cast<std::size_t>(idx)] ^= v;
        }
      }
    });
  }

  for (auto& t : producers) t.join();
  queue.Close();
  for (auto& t : consumers) t.join();

  const auto t1 = std::chrono::steady_clock::now();
  const long ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  long consumed = shared_consumed;
  std::uint64_t checksum = shared_checksum;
  if (!cfg.buggy) {
    consumed = 0;
    checksum = 0;
    for (long i = 0; i < cfg.consumers; ++i) {
      consumed += local_counts[static_cast<std::size_t>(i)];
      checksum ^= local_sums[static_cast<std::size_t>(i)];
    }
  }

  std::println("workq: produced={} consumed={} checksum={:016x} ms={}",
               produced.load(), consumed, checksum, ms);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  auto cfg = Parse(argc, argv);
  if (!cfg) {
    if (!cfg.error().message.empty()) {
      std::println(stderr, "workq: {}", cfg.error().message);
    }
    std::println(stderr, "{}", kUsage);
    return 2;
  }
  return Run(*cfg);
}
