// chatterd-fastpath — a stripped, pinned, allocation-free derivative of the
// chatterd echo/pub-sub hot path (chapter 21) built purely to answer one
// question: what does a low-latency network hot path actually cost, and what
// does removing that cost look like at the syscall level?
//
// Three subcommands, one 64-byte wire frame:
//
//   app fastpath --port P --pin CPU [--busy-poll]
//       Low-latency echo server. sched_setaffinity(2) pins this (single,
//       unthreaded) process to CPU; one stack-allocated 64-byte buffer is
//       reused for every message (zero heap allocation in the loop);
//       --busy-poll spins a non-blocking recv(2) instead of blocking read(2),
//       trading 100% of the pinned core for the removal of the sleep/wake
//       round trip through the scheduler.
//
//   app naive --port P
//       Ordinary echo server: default scheduling (no affinity), a fresh heap
//       buffer allocated per message, blocking read(2). The baseline chatterd
//       hot loop would do exactly this.
//
//   app measure --target HOST:PORT --n N [--warmup W]
//       Connect, then run N synchronous ping-pong round trips (send frame,
//       block for the echo, repeat — never more than one frame in flight) and
//       print a p50/p90/p99/p99.9 latency table in nanoseconds. W warmup
//       round trips are discarded before recording starts (they pay for TCP
//       slow-start / first-touch page faults / connection setup, none of
//       which are what this is measuring).
//
// THE FASTPATH FRAME (fixed 64 bytes; this example's own wire format, a
// deliberately smaller cousin of the chatterd CHAT FRAME from ch21 — no
// variable-length payload, so nothing here ever needs to size a buffer at
// runtime):
//
//     +-------+---------+------+-----------+-----------------+---------+
//     | magic | version | type | seq (u64) | send_ns (u64)   | pad     |
//     | 2B    | 1B      | 1B   | 8B BE     | 8B BE           | 44B     |
//     +-------+---------+------+-----------+-----------------+---------+
//
// magic is "CF" (chatterd-fastpath); the server never inspects seq/send_ns —
// it reads 64 bytes and writes the identical 64 bytes back. All timing is
// done by the client against its own clock; seq/send_ns are echoed back only
// so the client can confirm it got its own frame back rather than another
// client's or garbage (a correctness check, not a timing mechanism).
//
// C++23: RAII Socket owns every fd; std::expected carries fallible setup
// steps; std::atomic<bool> + a non-restarting SIGINT/SIGTERM handler unblock
// the accept wait and the read in progress, mirroring chatterd's
// stop_callback-via-shutdown(2) pattern without needing a second thread — the
// whole point of this example is that the hot loop runs on exactly one
// thread, so a hand-off to a jthread is deliberately not in the loop.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sched.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr std::size_t kFrameSize = 64;
constexpr std::uint8_t kMagic0 = 0x43;  // 'C'
constexpr std::uint8_t kMagic1 = 0x46;  // 'F'
constexpr std::uint8_t kWireVersion = 0x01;
constexpr std::uint8_t kTypeEcho = 0x01;

using Frame = std::array<std::uint8_t, kFrameSize>;

[[nodiscard]] std::string errno_text() {
    return std::error_code{errno, std::system_category()}.message();
}

template <typename T>
using Result = std::expected<T, std::string>;

void put_u64be(std::uint8_t* p, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[7 - i] = static_cast<std::uint8_t>((v >> (i * 8)) & 0xff);
    }
}

std::uint64_t get_u64be(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

Frame build_frame(std::uint64_t seq, std::uint64_t send_ns) {
    Frame f{};
    f[0] = kMagic0;
    f[1] = kMagic1;
    f[2] = kWireVersion;
    f[3] = kTypeEcho;
    put_u64be(f.data() + 4, seq);
    put_u64be(f.data() + 12, send_ns);
    // bytes 20..63 stay zero (aggregate init already zeroed them)
    return f;
}

bool frame_header_ok(const Frame& f) {
    return f[0] == kMagic0 && f[1] == kMagic1 && f[2] == kWireVersion && f[3] == kTypeEcho;
}

std::uint64_t now_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

// ---------------------------------------------------------------------------
// RAII Socket (same shape as chapter 21's).
// ---------------------------------------------------------------------------

class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_{fd} {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& o) noexcept : fd_{std::exchange(o.fd_, -1)} {}
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    ~Socket() { reset(); }

    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = -1;
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

private:
    int fd_ = -1;
};

Result<Socket> listen_tcp(std::uint16_t port) {
    Socket sock{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!sock.valid()) {
        return std::unexpected(std::format("socket: {}", errno_text()));
    }
    int one = 1;
    ::setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(sock.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        return std::unexpected(std::format("bind 0.0.0.0:{}: {}", port, errno_text()));
    }
    if (::listen(sock.get(), 16) < 0) {
        return std::unexpected(std::format("listen: {}", errno_text()));
    }
    return sock;
}

Result<Socket> connect_tcp(const std::string& host, std::uint16_t port) {
    Socket sock{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!sock.valid()) {
        return std::unexpected(std::format("socket: {}", errno_text()));
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return std::unexpected(std::format("{}: not an IPv4 address", host));
    }
    if (::connect(sock.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        return std::unexpected(std::format("connect {}:{}: {}", host, port, errno_text()));
    }
    return sock;
}

// Blocking read of exactly n bytes; false on EOF, hard error, or a signal
// landing (EINTR) — the caller decides whether that means "stop".
bool read_full_blocking(int fd, void* buf, std::size_t n) {
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t off = 0;
    while (off < n) {
        ssize_t r = ::read(fd, p + off, n - off);
        if (r < 0) {
            return false;
        }
        if (r == 0) {
            return false;  // peer closed
        }
        off += static_cast<std::size_t>(r);
    }
    return true;
}

// Non-blocking spin read: the socket must already be O_NONBLOCK. Spins on
// EAGAIN with no syscall other than recv(2) itself — no sched_yield, no
// nanosleep — which is the entire point of "busy-poll".
std::atomic<bool>* g_stop_for_busy_read = nullptr;

bool read_full_busy(int fd, void* buf, std::size_t n) {
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t off = 0;
    while (off < n) {
        ssize_t r = ::recv(fd, p + off, n - off, 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (g_stop_for_busy_read && g_stop_for_busy_read->load(std::memory_order_relaxed)) {
                    return false;
                }
                continue;  // spin
            }
            return false;
        }
        if (r == 0) {
            return false;  // peer closed
        }
        off += static_cast<std::size_t>(r);
    }
    return true;
}

void write_all(int fd, const void* buf, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    std::size_t off = 0;
    while (off < n) {
        ssize_t w = ::write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;  // peer gone; nothing sensible to do
        }
        off += static_cast<std::size_t>(w);
    }
}

// ---------------------------------------------------------------------------
// Shutdown plumbing: a plain (non-SA_RESTART) signal handler so a blocking
// read(2)/accept-wait poll(2) in progress is interrupted with EINTR rather
// than transparently resumed.
// ---------------------------------------------------------------------------

std::atomic<bool> g_stop{false};

extern "C" void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sa.sa_flags = 0;  // deliberately NOT SA_RESTART
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
}

Result<void> pin_to_cpu(int cpu) {
    long nproc = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu < 0 || (nproc > 0 && cpu >= nproc)) {
        return std::unexpected(
            std::format("cpu {} out of range (0..{})", cpu, nproc > 0 ? nproc - 1 : 0));
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
        return std::unexpected(std::format("sched_setaffinity({}): {}", cpu, errno_text()));
    }
    return {};
}

// ---------------------------------------------------------------------------
// fastpath / naive servers
// ---------------------------------------------------------------------------

int cmd_fastpath(std::uint16_t port, int pin_cpu, bool busy_poll) {
    if (auto pinned = pin_to_cpu(pin_cpu); !pinned) {
        std::println(stderr, "app: error: {}", pinned.error());
        return 1;
    }
    auto lsock = listen_tcp(port);
    if (!lsock) {
        std::println(stderr, "app: error: {}", lsock.error());
        return 1;
    }
    install_signal_handlers();
    g_stop_for_busy_read = &g_stop;
    std::println(stderr, "app: fastpath listening on 0.0.0.0:{} pinned-cpu={} busy-poll={}",
                 port, pin_cpu, busy_poll ? "on" : "off");

    while (!g_stop.load(std::memory_order_relaxed)) {
        pollfd pfd{lsock->get(), POLLIN, 0};
        int pr = ::poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            std::println(stderr, "app: error: poll: {}", errno_text());
            return 1;
        }
        if (pr == 0 || !(pfd.revents & POLLIN)) {
            continue;
        }
        Socket conn{::accept4(lsock->get(), nullptr, nullptr, SOCK_CLOEXEC)};
        if (!conn.valid()) {
            if (errno == EINTR || errno == EAGAIN) continue;
            std::println(stderr, "app: error: accept: {}", errno_text());
            return 1;
        }
        int one = 1;
        ::setsockopt(conn.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (busy_poll) {
            int flags = ::fcntl(conn.get(), F_GETFL, 0);
            ::fcntl(conn.get(), F_SETFL, flags | O_NONBLOCK);
        }

        // The ONE buffer for the whole life of this connection: no malloc,
        // no new, no make_unique anywhere in the loop below.
        Frame buf{};
        for (;;) {
            bool ok = busy_poll ? read_full_busy(conn.get(), buf.data(), kFrameSize)
                                : read_full_blocking(conn.get(), buf.data(), kFrameSize);
            if (!ok) break;
            write_all(conn.get(), buf.data(), kFrameSize);
        }
    }
    std::println(stderr, "app: fastpath shutting down");
    return 0;
}

int cmd_naive(std::uint16_t port) {
    auto lsock = listen_tcp(port);
    if (!lsock) {
        std::println(stderr, "app: error: {}", lsock.error());
        return 1;
    }
    install_signal_handlers();
    std::println(stderr, "app: naive listening on 0.0.0.0:{}", port);

    while (!g_stop.load(std::memory_order_relaxed)) {
        pollfd pfd{lsock->get(), POLLIN, 0};
        int pr = ::poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            std::println(stderr, "app: error: poll: {}", errno_text());
            return 1;
        }
        if (pr == 0 || !(pfd.revents & POLLIN)) {
            continue;
        }
        Socket conn{::accept4(lsock->get(), nullptr, nullptr, SOCK_CLOEXEC)};
        if (!conn.valid()) {
            if (errno == EINTR || errno == EAGAIN) continue;
            std::println(stderr, "app: error: accept: {}", errno_text());
            return 1;
        }
        for (;;) {
            // The "naive" part: a fresh heap buffer, every single message.
            auto heap_buf = std::make_unique<std::uint8_t[]>(kFrameSize);
            if (!read_full_blocking(conn.get(), heap_buf.get(), kFrameSize)) break;
            write_all(conn.get(), heap_buf.get(), kFrameSize);
            // heap_buf frees here, every iteration.
        }
    }
    std::println(stderr, "app: naive shutting down");
    return 0;
}

// ---------------------------------------------------------------------------
// measure
// ---------------------------------------------------------------------------

std::uint64_t percentile_ns(const std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    std::size_t n = sorted.size();
    std::size_t idx = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(n)));
    if (idx == 0) idx = 1;
    if (idx > n) idx = n;
    return sorted[idx - 1];
}

int cmd_measure(const std::string& host, std::uint16_t port, std::uint64_t n, std::uint64_t warmup,
                 const std::string& tag) {
    auto conn = connect_tcp(host, port);
    if (!conn) {
        std::println(stderr, "app: error: {}", conn.error());
        return 1;
    }
    int one = 1;
    ::setsockopt(conn->get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    std::println("app: measure target={}:{} n={} warmup={}", host, port, n, warmup);

    std::vector<std::uint64_t> samples;
    samples.reserve(n);
    const std::uint64_t total = n + warmup;
    for (std::uint64_t i = 0; i < total; ++i) {
        const Frame out = build_frame(i, 0);
        const std::uint64_t t_send = now_ns();
        write_all(conn->get(), out.data(), kFrameSize);
        Frame in{};
        if (!read_full_blocking(conn->get(), in.data(), kFrameSize)) {
            std::println(stderr, "app: error: measure: connection closed at iteration {}", i);
            return 1;
        }
        const std::uint64_t t_recv = now_ns();
        if (!frame_header_ok(in) || get_u64be(in.data() + 4) != i) {
            std::println(stderr, "app: error: measure: malformed/mismatched echo at iteration {}",
                         i);
            return 1;
        }
        if (i >= warmup) {
            samples.push_back(t_recv - t_send);
        }
    }

    std::ranges::sort(samples);
    const std::uint64_t p50 = percentile_ns(samples, 50.0);
    const std::uint64_t p90 = percentile_ns(samples, 90.0);
    const std::uint64_t p99 = percentile_ns(samples, 99.0);
    const std::uint64_t p999 = percentile_ns(samples, 99.9);
    const std::uint64_t mn = samples.front();
    const std::uint64_t mx = samples.back();
    double sum = 0;
    for (auto v : samples) sum += static_cast<double>(v);
    const double mean = sum / static_cast<double>(samples.size());

    std::println(
        "app: percentiles_ns tag={} p50={} p90={} p99={} p99.9={} min={} max={} mean={:.2f} n={}",
        tag.empty() ? "-" : tag, p50, p90, p99, p999, mn, mx, mean, samples.size());
    std::println("app: table");
    std::println("  p50    {} ns", p50);
    std::println("  p90    {} ns", p90);
    std::println("  p99    {} ns", p99);
    std::println("  p99.9  {} ns", p999);
    std::println("  min    {} ns", mn);
    std::println("  max    {} ns", mx);
    std::println("  mean   {:.2f} ns", mean);
    return 0;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

int usage() {
    std::println(stderr, "usage: app fastpath --port P --pin CPU [--busy-poll]");
    std::println(stderr, "       app naive --port P");
    std::println(stderr, "       app measure --target HOST:PORT --n N [--warmup W] [--tag TAG]");
    return 2;
}

std::optional<std::uint16_t> parse_port(std::string_view s) {
    std::uint64_t v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size() || v == 0 || v > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(v);
}

std::optional<std::uint64_t> parse_u64(std::string_view s) {
    std::uint64_t v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        return usage();
    }
    const std::string sub = args[0];

    if (sub == "fastpath") {
        std::string port_str;
        int pin_cpu = -1;
        bool busy_poll = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--port" && i + 1 < args.size()) {
                port_str = args[++i];
            } else if (args[i] == "--pin" && i + 1 < args.size()) {
                auto v = parse_u64(args[++i]);
                if (!v) return usage();
                pin_cpu = static_cast<int>(*v);
            } else if (args[i] == "--busy-poll") {
                busy_poll = true;
            } else {
                return usage();
            }
        }
        auto port = parse_port(port_str);
        if (!port || pin_cpu < 0) return usage();
        return cmd_fastpath(*port, pin_cpu, busy_poll);
    }

    if (sub == "naive") {
        std::string port_str;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--port" && i + 1 < args.size()) {
                port_str = args[++i];
            } else {
                return usage();
            }
        }
        auto port = parse_port(port_str);
        if (!port) return usage();
        return cmd_naive(*port);
    }

    if (sub == "measure") {
        std::string target;
        std::uint64_t n = 0;
        std::uint64_t warmup = 200;
        std::string tag;
        bool have_n = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--target" && i + 1 < args.size()) {
                target = args[++i];
            } else if (args[i] == "--n" && i + 1 < args.size()) {
                auto v = parse_u64(args[++i]);
                if (!v) return usage();
                n = *v;
                have_n = true;
            } else if (args[i] == "--warmup" && i + 1 < args.size()) {
                auto v = parse_u64(args[++i]);
                if (!v) return usage();
                warmup = *v;
            } else if (args[i] == "--tag" && i + 1 < args.size()) {
                tag = args[++i];
            } else {
                return usage();
            }
        }
        auto colon = target.rfind(':');
        if (!have_n || n == 0 || colon == std::string::npos) return usage();
        auto port = parse_port(std::string_view(target).substr(colon + 1));
        if (!port) return usage();
        std::string host = target.substr(0, colon);
        return cmd_measure(host, *port, n, warmup, tag);
    }

    return usage();
}
