// shmkv v1 — cross-process coordination over a shared-memory key-value file.
//
// The v0 file format (fixed slots in an mmap'd file) grows a v1 header with
// SHKV2 magic:
//
//   offset  size  field
//   0       8     magic "SHKV2\0\0\0"
//   8       4     seqlock word   (u32; odd = writer in critical section)
//   12      4     futex word     (u32; low 32 bits of the update counter)
//   16      8     update counter (u64; total updates published)
//   24      40    reserved
//   64      512   8 slots x 64 bytes: key[24] NUL-padded, value[40] NUL-padded
//
// Three coordination channels are demonstrated:
//   serve  — writer: seqlock-protected publish + FUTEX_WAKE after each update
//   watch  — reader: FUTEX_WAIT loop + seqlock-consistent snapshot reads
//   bench  — latency shoot-out: futex wake vs POSIX message queue vs 1 ms
//            sleep-polling of the counter
//
// Seqlock protocol (single writer):
//   write: seq += 1 (relaxed) ; release fence ; mutate data ; seq += 1 (release)
//   read:  s1 = seq (acquire); if odd retry; copy data; acquire fence;
//          s2 = seq (relaxed); consistent iff s1 == s2
// The data copy itself is a plain memcpy raced against the writer; the retry
// loop discards any torn copy, which is the standard seqlock trade-off.
//
// Everything is RAII (fd, mapping, message queue), errors travel as
// std::expected until main turns them into exit codes.

#include <linux/futex.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <expected>
#include <fcntl.h>
#include <memory>
#include <mqueue.h>
#include <print>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr std::array<unsigned char, 8> kMagic{'S', 'H', 'K', 'V', '2', 0, 0, 0};
constexpr std::size_t kFileSize = 4096;
constexpr std::size_t kOffSeq = 8;
constexpr std::size_t kOffFutex = 12;
constexpr std::size_t kOffCounter = 16;
constexpr std::size_t kOffSlots = 64;
constexpr std::size_t kSlotCount = 8;
constexpr std::size_t kKeySize = 24;
constexpr std::size_t kValSize = 40;
constexpr std::size_t kSlotSize = 64;
constexpr std::size_t kSlotsBytes = kSlotCount * kSlotSize;

std::string errno_text(int err) { return std::strerror(err); }

// --- RAII wrappers ---------------------------------------------------------

class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
  Fd& operator=(Fd&& o) noexcept {
    if (this != &o) {
      reset();
      fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() { reset(); }
  void reset() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }
  int get() const { return fd_; }

 private:
  int fd_ = -1;
};

class Mapping {
 public:
  Mapping() = default;
  Mapping(void* p, std::size_t len) : p_(p), len_(len) {}
  Mapping(Mapping&& o) noexcept
      : p_(std::exchange(o.p_, nullptr)), len_(std::exchange(o.len_, 0)) {}
  Mapping& operator=(Mapping&& o) noexcept {
    if (this != &o) {
      reset();
      p_ = std::exchange(o.p_, nullptr);
      len_ = std::exchange(o.len_, 0);
    }
    return *this;
  }
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  ~Mapping() { reset(); }
  void reset() {
    if (p_ != nullptr) ::munmap(p_, len_);
    p_ = nullptr;
  }
  unsigned char* bytes() const { return static_cast<unsigned char*>(p_); }

 private:
  void* p_ = nullptr;
  std::size_t len_ = 0;
};

// mq_close + mq_unlink on scope exit — no queue name survives a bench run.
class MessageQueue {
 public:
  MessageQueue(mqd_t d, std::string name) : d_(d), name_(std::move(name)) {}
  MessageQueue(const MessageQueue&) = delete;
  MessageQueue& operator=(const MessageQueue&) = delete;
  ~MessageQueue() {
    ::mq_close(d_);
    ::mq_unlink(name_.c_str());
  }
  mqd_t get() const { return d_; }

 private:
  mqd_t d_;
  std::string name_;
};

// --- the shared file -------------------------------------------------------

struct Shm {
  Fd fd;
  Mapping map;

  unsigned char* base() const { return map.bytes(); }
  std::atomic_ref<std::uint32_t> seq() const {
    return std::atomic_ref{*reinterpret_cast<std::uint32_t*>(base() + kOffSeq)};
  }
  std::atomic_ref<std::uint32_t> futex_word() const {
    return std::atomic_ref{*reinterpret_cast<std::uint32_t*>(base() + kOffFutex)};
  }
  std::uint32_t* futex_ptr() const {
    return reinterpret_cast<std::uint32_t*>(base() + kOffFutex);
  }
  std::atomic_ref<std::uint64_t> counter() const {
    return std::atomic_ref{*reinterpret_cast<std::uint64_t*>(base() + kOffCounter)};
  }
};

std::expected<Shm, std::string> open_shm(const std::string& path, bool create) {
  int flags = O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0);
  Fd fd(::open(path.c_str(), flags, 0644));
  if (fd.get() < 0) {
    return std::unexpected(std::format("open {}: {}", path, errno_text(errno)));
  }
  if (create) {
    if (::ftruncate(fd.get(), static_cast<off_t>(kFileSize)) != 0) {
      return std::unexpected(std::format("ftruncate {}: {}", path, errno_text(errno)));
    }
  } else {
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) {
      return std::unexpected(std::format("fstat {}: {}", path, errno_text(errno)));
    }
    if (st.st_size < static_cast<off_t>(kFileSize)) {
      return std::unexpected(std::format("{}: bad magic (want SHKV2)", path));
    }
  }
  void* p = ::mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
  if (p == MAP_FAILED) {
    return std::unexpected(std::format("mmap {}: {}", path, errno_text(errno)));
  }
  Shm shm{std::move(fd), Mapping{p, kFileSize}};
  if (create) {
    std::memset(shm.base(), 0, kFileSize);
    std::memcpy(shm.base(), kMagic.data(), kMagic.size());
  } else if (std::memcmp(shm.base(), kMagic.data(), kMagic.size()) != 0) {
    return std::unexpected(std::format("{}: bad magic (want SHKV2)", path));
  }
  return shm;
}

// --- seqlock ---------------------------------------------------------------

template <typename Mutate>
void seqlock_write(const Shm& shm, Mutate&& mutate) {
  auto seq = shm.seq();
  seq.fetch_add(1, std::memory_order_relaxed);  // odd: writer active
  std::atomic_thread_fence(std::memory_order_release);
  mutate();
  seq.fetch_add(1, std::memory_order_release);  // even: consistent again
}

struct Snapshot {
  std::uint64_t counter = 0;
  std::array<unsigned char, kSlotsBytes> slots{};
};

std::expected<Snapshot, std::string> seqlock_read(const Shm& shm) {
  auto seq = shm.seq();
  for (int attempt = 0; attempt < 1'000'000; ++attempt) {
    std::uint32_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1) {  // writer mid-update: retry
      std::this_thread::yield();
      continue;
    }
    Snapshot snap;
    snap.counter = shm.counter().load(std::memory_order_relaxed);
    std::memcpy(snap.slots.data(), shm.base() + kOffSlots, kSlotsBytes);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq.load(std::memory_order_relaxed) == s1) return snap;
  }
  return std::unexpected(std::string("seqlock read livelocked"));
}

// --- futex -----------------------------------------------------------------

// Shared (non-PRIVATE) futex: the kernel keys on the file's inode, so waiters
// and wakers in different processes rendezvous through the same mapping.
void futex_wait(std::uint32_t* addr, std::uint32_t expected,
                std::chrono::milliseconds timeout) {
  timespec ts{.tv_sec = timeout.count() / 1000,
              .tv_nsec = (timeout.count() % 1000) * 1'000'000};
  // EAGAIN (word already moved on), ETIMEDOUT and EINTR are all normal here:
  // the caller re-checks shared state and decides whether to wait again.
  ::syscall(SYS_futex, addr, FUTEX_WAIT, expected, &ts, nullptr, 0);
}

void futex_wake_all(std::uint32_t* addr) {
  ::syscall(SYS_futex, addr, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

// --- helpers ---------------------------------------------------------------

std::string cstr(const unsigned char* p, std::size_t n) {
  std::size_t len = 0;
  while (len < n && p[len] != 0) ++len;
  return std::string(reinterpret_cast<const char*>(p), len);
}

void write_slot(const Shm& shm, std::uint64_t id, std::string_view key,
                std::string_view val) {
  unsigned char* slot = shm.base() + kOffSlots + ((id - 1) % kSlotCount) * kSlotSize;
  std::memset(slot, 0, kSlotSize);
  std::memcpy(slot, key.data(), std::min(key.size(), kKeySize - 1));
  std::memcpy(slot + kKeySize, val.data(), std::min(val.size(), kValSize - 1));
}

std::pair<std::string, std::string> read_slot(const Snapshot& snap, std::uint64_t id) {
  const unsigned char* slot = snap.slots.data() + ((id - 1) % kSlotCount) * kSlotSize;
  return {cstr(slot, kKeySize), cstr(slot + kKeySize, kValSize)};
}

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// --- serve -----------------------------------------------------------------

int cmd_serve(const std::string& path, std::uint64_t updates, std::uint64_t interval_ms) {
  auto shm = open_shm(path, /*create=*/true);
  if (!shm) {
    std::println(stderr, "shmkv: {}", shm.error());
    return 1;
  }
  std::println("serve: file={} updates={} interval_ms={}", path, updates, interval_ms);
  for (std::uint64_t k = 1; k <= updates; ++k) {
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    std::string key = std::format("k{}", k);
    std::string val = std::format("value-{}", k);
    seqlock_write(*shm, [&] {
      write_slot(*shm, k, key, val);
      shm->counter().store(k, std::memory_order_relaxed);
    });
    shm->futex_word().store(static_cast<std::uint32_t>(k), std::memory_order_release);
    futex_wake_all(shm->futex_ptr());
    std::println("published update {}: {}={}", k, key, val);
  }
  std::println("serve: complete updates={}", updates);
  return 0;
}

// --- watch -----------------------------------------------------------------

int cmd_watch(const std::string& path, std::uint64_t events) {
  auto shm = open_shm(path, /*create=*/false);
  if (!shm) {
    std::println(stderr, "shmkv: {}", shm.error());
    return 1;
  }
  std::println("watch: file={} events={}", path, events);
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  std::uint64_t last = 0;
  std::uint64_t printed = 0;
  while (printed < events) {
    if (std::chrono::steady_clock::now() > deadline) {
      std::println(stderr, "shmkv: watch: timed out waiting for updates");
      return 1;
    }
    auto snap = seqlock_read(*shm);
    if (!snap) {
      std::println(stderr, "shmkv: {}", snap.error());
      return 1;
    }
    if (snap->counter > last) {
      // The slot ring holds the last kSlotCount updates, so a watcher that
      // slept through a wake can still back-fill every missed id.
      for (std::uint64_t id = last + 1; id <= snap->counter && printed < events; ++id) {
        auto [key, val] = read_slot(*snap, id);
        std::println("observed update {}: {}={}", id, key, val);
        ++printed;
      }
      last = snap->counter;
      continue;
    }
    std::uint32_t w = shm->futex_word().load(std::memory_order_acquire);
    if (w != static_cast<std::uint32_t>(last)) continue;  // moved on: re-read now
    futex_wait(shm->futex_ptr(), w, 2000ms);
  }
  std::println("watch: complete events={}", events);
  return 0;
}

// --- bench -----------------------------------------------------------------

enum class Channel { kFutex, kMq, kPoll };

timespec abs_deadline(std::chrono::seconds ahead) {
  timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ahead.count();
  return ts;
}

int cmd_bench(const std::string& path, std::uint64_t rounds, Channel channel,
              const std::string& channel_name) {
  auto shm = open_shm(path, /*create=*/true);
  if (!shm) {
    std::println(stderr, "shmkv: {}", shm.error());
    return 1;
  }

  std::unique_ptr<MessageQueue> mq;
  if (channel == Channel::kMq) {
    std::string name = std::format("/shmkv-bench-{}", ::getpid());
    mq_attr attr{};
    attr.mq_maxmsg = 8;
    attr.mq_msgsize = 8;
    mqd_t d = ::mq_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
    if (d == static_cast<mqd_t>(-1)) {
      std::println(stderr, "shmkv: mq_open {}: {}", name, errno_text(errno));
      return 1;
    }
    mq = std::make_unique<MessageQueue>(d, name);
  }

  std::vector<std::int64_t> send_ns(rounds), recv_ns(rounds);
  std::atomic<std::uint64_t> ack{0};

  std::jthread consumer([&] {
    for (std::uint64_t i = 1; i <= rounds; ++i) {
      switch (channel) {
        case Channel::kFutex:
          for (;;) {
            std::uint32_t w = shm->futex_word().load(std::memory_order_acquire);
            if (w >= static_cast<std::uint32_t>(i)) break;
            futex_wait(shm->futex_ptr(), w, 200ms);
          }
          break;
        case Channel::kPoll:
          // Sleep FIRST, then look: by construction every observation costs
          // at least one full 1 ms nap — the cost futex wakeups avoid.
          for (;;) {
            std::this_thread::sleep_for(1ms);
            if (shm->counter().load(std::memory_order_acquire) >= i) break;
          }
          break;
        case Channel::kMq: {
          char buf[8];
          for (;;) {
            timespec ts = abs_deadline(10s);
            ssize_t n = ::mq_timedreceive(mq->get(), buf, sizeof buf, nullptr, &ts);
            if (n == 8) break;
            if (n < 0 && errno == EINTR) continue;
            std::println(stderr, "shmkv: mq_timedreceive: {}", errno_text(errno));
            std::exit(1);
          }
          break;
        }
      }
      recv_ns[i - 1] = now_ns();
      ack.store(i, std::memory_order_release);
    }
  });

  const auto spin_deadline = std::chrono::steady_clock::now() + 30s;
  for (std::uint64_t i = 1; i <= rounds; ++i) {
    // Spin until the consumer acknowledged round i-1 so rounds never overlap
    // (an overlapped round would measure queueing, not wakeup latency).
    while (ack.load(std::memory_order_acquire) != i - 1) {
      if (std::chrono::steady_clock::now() > spin_deadline) {
        std::println(stderr, "shmkv: bench: consumer stalled");
        std::exit(1);
      }
      std::this_thread::yield();
    }
    send_ns[i - 1] = now_ns();
    if (channel == Channel::kMq) {
      char buf[8];
      std::uint64_t id = i;
      std::memcpy(buf, &id, sizeof buf);
      timespec ts = abs_deadline(10s);
      if (::mq_timedsend(mq->get(), buf, sizeof buf, 0, &ts) != 0) {
        std::println(stderr, "shmkv: mq_timedsend: {}", errno_text(errno));
        std::exit(1);
      }
    } else {
      seqlock_write(*shm, [&] { shm->counter().store(i, std::memory_order_relaxed); });
      shm->futex_word().store(static_cast<std::uint32_t>(i), std::memory_order_release);
      if (channel == Channel::kFutex) futex_wake_all(shm->futex_ptr());
    }
  }
  consumer.join();

  std::vector<std::int64_t> us(rounds);
  for (std::uint64_t i = 0; i < rounds; ++i) {
    us[i] = std::max<std::int64_t>(0, recv_ns[i] - send_ns[i]) / 1000;
  }
  std::ranges::sort(us);
  auto pct = [&](std::uint64_t p) {
    std::uint64_t idx = rounds * p / 100;
    if (idx >= rounds) idx = rounds - 1;
    return us[idx];
  };
  std::println("bench: channel={} rounds={} p50_us={} p99_us={}", channel_name,
               rounds, pct(50), pct(99));
  return 0;
}

// --- CLI -------------------------------------------------------------------

int usage() {
  std::println(stderr, "usage: shmkv serve FILE [--updates N] [--interval-ms T]");
  std::println(stderr, "       shmkv watch FILE [--events N]");
  std::println(stderr, "       shmkv bench FILE [--rounds N] [--channel futex|mq|poll]");
  return 2;
}

std::expected<std::uint64_t, std::string> parse_u64(std::string_view s) {
  std::uint64_t v = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || ptr != s.data() + s.size() || v == 0 || v > 1'000'000) {
    return std::unexpected(std::format("bad number: {}", s));
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.size() < 2) return usage();
  const std::string& cmd = args[0];
  const std::string& file = args[1];

  std::uint64_t updates = 8, interval_ms = 100, events = 4, rounds = 100;
  std::string channel_name = "futex";
  for (std::size_t i = 2; i < args.size(); i += 2) {
    if (i + 1 >= args.size()) return usage();
    const std::string& flag = args[i];
    const std::string& val = args[i + 1];
    if (flag == "--channel" && cmd == "bench") {
      channel_name = val;
      continue;
    }
    std::expected<std::uint64_t, std::string> n = parse_u64(val);
    if (!n) return usage();
    if (flag == "--updates" && cmd == "serve") {
      updates = *n;
    } else if (flag == "--interval-ms" && cmd == "serve") {
      interval_ms = *n;
    } else if (flag == "--events" && cmd == "watch") {
      events = *n;
    } else if (flag == "--rounds" && cmd == "bench") {
      rounds = *n;
    } else {
      return usage();
    }
  }

  if (cmd == "serve") return cmd_serve(file, updates, interval_ms);
  if (cmd == "watch") return cmd_watch(file, events);
  if (cmd == "bench") {
    Channel ch{};
    if (channel_name == "futex") {
      ch = Channel::kFutex;
    } else if (channel_name == "mq") {
      ch = Channel::kMq;
    } else if (channel_name == "poll") {
      ch = Channel::kPoll;
    } else {
      return usage();
    }
    return cmd_bench(file, rounds, ch, channel_name);
  }
  return usage();
}
