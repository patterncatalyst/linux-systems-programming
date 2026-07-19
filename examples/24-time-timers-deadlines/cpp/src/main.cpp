// chatterd v3 — heartbeats + deadlines (chapter 24: time, timers, deadlines).
//
//   chatterd clockprobe
//   chatterd listen  --name N --addr HOST:PORT   [opts]
//   chatterd connect --name N --peer NAME@HOST:PORT [opts]
//
// The C++ take on deadlines: a periodic timerfd(CLOCK_MONOTONIC) drives the
// keepalive heartbeat, and a std::chrono::steady_clock deadline drives peer
// liveness. Both are fed to a single poll(2) loop alongside the peer socket
// and a signalfd(2) doorbell for clean shutdown. Nothing here reads the wall
// clock: a peer is declared dead by *silence* measured on a monotonic clock,
// never by a wall-clock timestamp that a settimeofday(2) could rewind.
//
// Wire format — this is THE canonical chatterd chat frame, unchanged since it
// was introduced in chapter 21 and extended only by adding TYPE values in
// later chapters; the header never changes. All integers big-endian:
//
//   byte 0   MAGIC0  = 0x43 ('C')
//   byte 1   MAGIC1  = 0x48 ('H')
//   byte 2   VERSION = 0x01
//   byte 3   TYPE    1=JOIN 2=MSG 3=DELIVER 4=WELCOME 5=PING
//   byte 4-5 LENGTH  uint16 payload length N
//   byte 6.. PAYLOAD N bytes: JOIN/MSG/WELCOME/PING are UTF-8 text (PING
//                             empty); DELIVER is name + 0x00 (NUL) + text.
//
// This v3 (chapter 24) peer only ever emits JOIN, MSG, and PING — DELIVER and
// WELCOME belong to the server-broadcast versions (ch22/ch27) and, since the
// header is shared, are simply treated as liveness traffic here if ever seen.
// PING is the v3 addition over v0/v1. Any received frame is "traffic" and
// resets the deadline; a peer with no traffic for --timeout-ms is dropped.

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <expected>
#include <format>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace {

using namespace std::chrono;

// ---------------------------------------------------------------------------
// RAII around a file descriptor — never a naked close(2).
// ---------------------------------------------------------------------------
class UniqueFd {
  public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~UniqueFd() { reset(); }
    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }

  private:
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    int fd_ = -1;
};

[[nodiscard]] std::error_code errno_ec() {
    return {errno, std::generic_category()};
}

[[noreturn]] void die(std::string_view what, std::error_code ec) {
    std::println(stderr, "chatterd: error: {}: {}", what, ec.message());
    std::exit(1);
}

[[noreturn]] void usage() {
    std::println(stderr, "usage: chatterd clockprobe");
    std::println(stderr, "       chatterd listen  --name N --addr HOST:PORT [opts]");
    std::println(stderr, "       chatterd connect --name N --peer NAME@HOST:PORT [opts]");
    std::println(stderr, "opts:  --heartbeat-ms H --timeout-ms T --backoff-ms B "
                         "--max-backoff-ms M --message MSG --seed S");
    std::exit(2);
}

// ---------------------------------------------------------------------------
// Protocol
// ---------------------------------------------------------------------------
constexpr std::uint8_t kMagic0 = 'C';
constexpr std::uint8_t kMagic1 = 'H';
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kHeader = 6;
constexpr std::size_t kMaxPayload = 65535;

// DELIVER (3) and WELCOME (4) belong to other chatterd versions; this v3
// peer never emits them and treats them as generic traffic if received.
enum class Type : std::uint8_t { Join = 1, Msg = 2, Deliver = 3, Welcome = 4, Ping = 5 };

struct Frame {
    Type type;
    std::string payload;
};

[[nodiscard]] std::string encode(Type type, std::string_view payload) {
    std::string out;
    out.resize(kHeader + payload.size());
    out[0] = static_cast<char>(kMagic0);
    out[1] = static_cast<char>(kMagic1);
    out[2] = static_cast<char>(kVersion);
    out[3] = static_cast<char>(type);
    out[4] = static_cast<char>((payload.size() >> 8) & 0xff);
    out[5] = static_cast<char>(payload.size() & 0xff);
    std::memcpy(out.data() + kHeader, payload.data(), payload.size());
    return out;
}

// Pop one complete frame off the front of buf. nullopt = need more bytes; a
// bad magic/version sets `bad` so the caller can drop the connection.
[[nodiscard]] std::optional<Frame> pop_frame(std::string& buf, bool& bad) {
    if (buf.size() < kHeader) {
        return std::nullopt;
    }
    const auto b = reinterpret_cast<const unsigned char*>(buf.data());
    if (b[0] != kMagic0 || b[1] != kMagic1 || b[2] != kVersion) {
        bad = true;
        return std::nullopt;
    }
    const std::size_t len = (static_cast<std::size_t>(b[4]) << 8) | b[5];
    if (buf.size() < kHeader + len) {
        return std::nullopt;
    }
    Frame f{static_cast<Type>(b[3]), buf.substr(kHeader, len)};
    buf.erase(0, kHeader + len);
    return f;
}

[[nodiscard]] std::expected<void, std::error_code> send_all(int fd, std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(errno_ec());
        }
        off += static_cast<std::size_t>(n);
    }
    return {};
}

[[nodiscard]] std::expected<void, std::error_code>
send_frame(int fd, Type type, std::string_view payload = {}) {
    return send_all(fd, encode(type, payload));
}

// ---------------------------------------------------------------------------
// Timerfd helper (CLOCK_MONOTONIC), periodic.
// ---------------------------------------------------------------------------
[[nodiscard]] UniqueFd periodic_timerfd(long period_ms) {
    UniqueFd tfd(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC));
    if (!tfd.valid()) {
        die("timerfd_create", errno_ec());
    }
    itimerspec its{};
    its.it_interval.tv_sec = period_ms / 1000;
    its.it_interval.tv_nsec = (period_ms % 1000) * 1'000'000;
    its.it_value = its.it_interval;
    if (::timerfd_settime(tfd.get(), 0, &its, nullptr) < 0) {
        die("timerfd_settime", errno_ec());
    }
    return tfd;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct Opts {
    std::string name;
    std::string listen_addr;
    std::string peer_name;
    std::string peer_addr;
    long heartbeat_ms = 1000;
    long timeout_ms = 3000;
    long backoff_ms = 200;
    long max_backoff_ms = 5000;
    std::string message;
    bool has_message = false;
    unsigned seed = 0;
    bool has_seed = false;
};

[[nodiscard]] std::expected<long, std::string> parse_long(std::string_view s, long min) {
    const std::string tmp(s);
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(tmp.c_str(), &end, 10);
    if (errno != 0 || end == tmp.c_str() || *end != '\0' || v < min) {
        return std::unexpected("bad numeric value: " + tmp);
    }
    return v;
}

// Split "HOST:PORT" -> {host, port}; the port keeps its string form so ":0"
// (an ephemeral bind) round-trips unchanged.
[[nodiscard]] std::expected<std::pair<std::string, std::string>, std::string>
split_host_port(std::string_view hp) {
    const auto colon = hp.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == hp.size()) {
        return std::unexpected("expected HOST:PORT, got " + std::string(hp));
    }
    return std::pair{std::string(hp.substr(0, colon)), std::string(hp.substr(colon + 1))};
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------
[[nodiscard]] std::expected<UniqueFd, std::error_code>
make_listener(const std::string& host, const std::string& port, std::string& bound_out) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
        return std::unexpected(errno_ec());
    }
    UniqueFd fd(::socket(res->ai_family, res->ai_socktype | SOCK_CLOEXEC, res->ai_protocol));
    if (!fd.valid()) {
        const auto ec = errno_ec();
        ::freeaddrinfo(res);
        return std::unexpected(ec);
    }
    int one = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (::bind(fd.get(), res->ai_addr, res->ai_addrlen) < 0 || ::listen(fd.get(), 8) < 0) {
        const auto ec = errno_ec();
        ::freeaddrinfo(res);
        return std::unexpected(ec);
    }
    ::freeaddrinfo(res);

    // Read back the actually-bound port (matters for an ephemeral ":0").
    sockaddr_in sin{};
    socklen_t sl = sizeof sin;
    ::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&sin), &sl);
    char ip[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &sin.sin_addr, ip, sizeof ip);
    bound_out = std::format("{}:{}", ip, ntohs(sin.sin_port));
    return fd;
}

[[nodiscard]] std::expected<UniqueFd, std::error_code>
dial(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
        return std::unexpected(errno_ec());
    }
    UniqueFd fd(::socket(res->ai_family, res->ai_socktype | SOCK_CLOEXEC, res->ai_protocol));
    if (!fd.valid()) {
        const auto ec = errno_ec();
        ::freeaddrinfo(res);
        return std::unexpected(ec);
    }
    const int rc = ::connect(fd.get(), res->ai_addr, res->ai_addrlen);
    const auto ec = errno_ec();
    ::freeaddrinfo(res);
    if (rc < 0) {
        return std::unexpected(ec);
    }
    return fd;
}

// ---------------------------------------------------------------------------
// Shutdown doorbell: SIGINT/SIGTERM turned into a pollable fd.
// ---------------------------------------------------------------------------
[[nodiscard]] UniqueFd make_shutdown_fd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
        die("sigprocmask", errno_ec());
    }
    UniqueFd fd(::signalfd(-1, &mask, SFD_CLOEXEC));
    if (!fd.valid()) {
        die("signalfd", errno_ec());
    }
    return fd;
}

// ---------------------------------------------------------------------------
// A single peer session over an established socket.
// ---------------------------------------------------------------------------
enum class Outcome { TimedOut, Shutdown, PeerError };

struct SessionResult {
    Outcome outcome;
    bool linked = false;  // did we complete a JOIN handshake at least once?
    std::string peer;     // name learned from the peer's JOIN, if any
};

// Drives one connection: sends JOIN, exchanges heartbeats, prints the linked
// line and any received messages, and returns when the peer goes silent
// (TimedOut), we are asked to quit (Shutdown), or the socket errors.
SessionResult run_session(const Opts& opts, UniqueFd sock, int shutdown_fd,
                          std::string peer_display) {
    UniqueFd hb = periodic_timerfd(opts.heartbeat_ms);

    if (auto r = send_frame(sock.get(), Type::Join, opts.name); !r) {
        return {Outcome::PeerError, false, peer_display};
    }

    std::string buf;
    bool linked = false;
    bool sock_dead = false;  // peer sent EOF; keep the deadline running
    const auto timeout = milliseconds(opts.timeout_ms);
    auto deadline = steady_clock::now() + timeout;

    for (;;) {
        int poll_ms = 0;
        {
            const auto left = deadline - steady_clock::now();
            poll_ms = left <= 0ns
                          ? 0
                          : static_cast<int>(duration_cast<milliseconds>(left).count()) + 1;
        }
        std::vector<pollfd> pfds;
        pfds.push_back({shutdown_fd, POLLIN, 0});
        pfds.push_back({hb.get(), POLLIN, 0});
        const bool watch_sock = !sock_dead;
        if (watch_sock) {
            pfds.push_back({sock.get(), POLLIN, 0});
        }

        const int n = ::poll(pfds.data(), pfds.size(), poll_ms);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("poll", errno_ec());
        }
        if (n == 0) {
            return {Outcome::TimedOut, linked, peer_display};  // deadline: peer went silent
        }
        if (pfds[0].revents & POLLIN) {
            return {Outcome::Shutdown, linked, peer_display};
        }
        if (pfds[1].revents & POLLIN) {
            std::uint64_t ticks = 0;
            [[maybe_unused]] auto rd = ::read(hb.get(), &ticks, sizeof ticks);
            if (!sock_dead) {
                (void)send_frame(sock.get(), Type::Ping);  // a broken pipe is left to the deadline
            }
        }
        if (watch_sock && (pfds[2].revents & (POLLIN | POLLHUP))) {
            char tmp[4096];
            const ssize_t rn = ::read(sock.get(), tmp, sizeof tmp);
            if (rn > 0) {
                buf.append(tmp, static_cast<std::size_t>(rn));
                deadline = steady_clock::now() + timeout;  // traffic: extend life
                bool bad = false;
                while (auto f = pop_frame(buf, bad)) {
                    if (f->type == Type::Join) {
                        peer_display = f->payload;
                        if (!linked) {
                            linked = true;
                            std::println("chatterd: {} linked with {}", opts.name, peer_display);
                            if (opts.has_message) {
                                (void)send_frame(sock.get(), Type::Msg, opts.message);
                            }
                        }
                    } else if (f->type == Type::Msg) {
                        std::println("chatterd: {} message from {}: {}", opts.name, peer_display,
                                     f->payload);
                    }
                    // PING / DELIVER / WELCOME / reserved: liveness only, already counted.
                }
                if (bad || buf.size() > kHeader + kMaxPayload) {
                    return {Outcome::PeerError, linked, peer_display};
                }
            } else {
                // EOF (0) or error: stop reading, let the deadline declare death.
                sock_dead = true;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Wait out a backoff, but wake early for shutdown. true = shut down now.
// ---------------------------------------------------------------------------
[[nodiscard]] bool sleep_or_shutdown(long ms, int shutdown_fd) {
    pollfd pfd{shutdown_fd, POLLIN, 0};
    for (;;) {
        const int n = ::poll(&pfd, 1, static_cast<int>(ms));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("poll", errno_ec());
        }
        return n > 0;  // readable = a shutdown signal arrived
    }
}

// ---------------------------------------------------------------------------
// Roles
// ---------------------------------------------------------------------------
int run_listen(const Opts& opts) {
    UniqueFd shutdown_fd = make_shutdown_fd();
    const auto hp = split_host_port(opts.listen_addr);
    if (!hp) {
        std::println(stderr, "chatterd: error: {}", hp.error());
        return 2;
    }
    std::string bound;
    auto listener = make_listener(hp->first, hp->second, bound);
    if (!listener) {
        die("listen", listener.error());
    }
    std::println("chatterd: {} listening on {}", opts.name, bound);

    std::string linked_peer = "peer";
    for (;;) {
        pollfd pfds[2] = {{shutdown_fd.get(), POLLIN, 0}, {listener->get(), POLLIN, 0}};
        const int n = ::poll(pfds, 2, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("poll", errno_ec());
        }
        if (pfds[0].revents & POLLIN) {
            std::println("chatterd: {} shutting down", opts.name);
            return 0;
        }
        if (pfds[1].revents & POLLIN) {
            UniqueFd conn(::accept4(listener->get(), nullptr, nullptr, SOCK_CLOEXEC));
            if (!conn.valid()) {
                continue;
            }
            const auto res = run_session(opts, std::move(conn), shutdown_fd.get(), linked_peer);
            if (res.outcome == Outcome::Shutdown) {
                std::println("chatterd: {} shutting down", opts.name);
                return 0;
            }
            if (res.outcome == Outcome::TimedOut && res.linked) {
                std::println("chatterd: peer {} timed out", res.peer);
            }
            // Loop back to accept: a reconnecting peer is welcomed again.
        }
    }
}

int run_connect(const Opts& opts) {
    UniqueFd shutdown_fd = make_shutdown_fd();
    const auto hp = split_host_port(opts.peer_addr);
    if (!hp) {
        std::println(stderr, "chatterd: error: {}", hp.error());
        return 2;
    }
    std::println("chatterd: {} connecting to {} at {}", opts.name, opts.peer_name, opts.peer_addr);

    std::mt19937 rng(opts.has_seed ? opts.seed : std::random_device{}());
    long backoff = opts.backoff_ms;

    for (;;) {
        auto sock = dial(hp->first, hp->second);
        if (sock) {
            backoff = opts.backoff_ms;  // a fresh TCP connection resets backoff
            const auto res = run_session(opts, std::move(*sock), shutdown_fd.get(), opts.peer_name);
            if (res.outcome == Outcome::Shutdown) {
                std::println("chatterd: {} shutting down", opts.name);
                return 0;
            }
            if (res.outcome == Outcome::TimedOut) {
                std::println("chatterd: peer {} timed out", opts.peer_name);
            }
        }
        // Jittered exponential backoff (equal jitter: half fixed, half random).
        const long half = backoff / 2;
        std::uniform_int_distribution<long> jit(0, half > 0 ? half : 0);
        const long delay = half + jit(rng);
        std::println("chatterd: reconnecting to {} in {}ms", opts.peer_name, delay);
        if (sleep_or_shutdown(delay, shutdown_fd.get())) {
            std::println("chatterd: {} shutting down", opts.name);
            return 0;
        }
        backoff = std::min(backoff * 2, opts.max_backoff_ms);
    }
}

int run_clockprobe() {
    timespec res{};
    ::clock_getres(CLOCK_MONOTONIC, &res);
    const long res_ns = res.tv_sec * 1'000'000'000L + res.tv_nsec;

    const auto t0 = steady_clock::now();
    timespec req{0, 1'000'000};  // 1 ms
    timespec rem{};
    while (::nanosleep(&req, &rem) != 0 && errno == EINTR) {
        req = rem;
    }
    const auto sleep_us = duration_cast<microseconds>(steady_clock::now() - t0).count();

    UniqueFd tfd(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC));
    if (!tfd.valid()) {
        die("timerfd_create", errno_ec());
    }
    itimerspec its{};
    its.it_value.tv_nsec = 1'000'000;  // one-shot 1 ms
    if (::timerfd_settime(tfd.get(), 0, &its, nullptr) < 0) {
        die("timerfd_settime", errno_ec());
    }
    const auto t1 = steady_clock::now();
    std::uint64_t ticks = 0;
    [[maybe_unused]] auto rd = ::read(tfd.get(), &ticks, sizeof ticks);
    const auto tfd_us = duration_cast<microseconds>(steady_clock::now() - t1).count();

    std::println("clockprobe: CLOCK_MONOTONIC res={}ns nanosleep(1ms) actual={}us "
                 "timerfd(1ms) actual={}us",
                 res_ns, sleep_us, tfd_us);
    return 0;
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------
[[nodiscard]] std::expected<Opts, std::string> parse_common(const std::vector<std::string>& args) {
    Opts o;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto need = [&](const std::string& flag) -> std::expected<std::string, std::string> {
            if (i + 1 >= args.size()) {
                return std::unexpected(flag + " needs a value");
            }
            return args[++i];
        };
        const std::string& a = args[i];
        if (a == "--name") {
            auto v = need(a);
            if (!v) return std::unexpected(v.error());
            o.name = *v;
        } else if (a == "--addr") {
            auto v = need(a);
            if (!v) return std::unexpected(v.error());
            o.listen_addr = *v;
        } else if (a == "--peer") {
            auto v = need(a);
            if (!v) return std::unexpected(v.error());
            const auto at = v->find('@');
            if (at == std::string::npos) {
                return std::unexpected("--peer wants NAME@HOST:PORT");
            }
            o.peer_name = v->substr(0, at);
            o.peer_addr = v->substr(at + 1);
        } else if (a == "--heartbeat-ms" || a == "--timeout-ms" || a == "--backoff-ms" ||
                   a == "--max-backoff-ms") {
            auto v = need(a);
            if (!v) return std::unexpected(v.error());
            auto num = parse_long(*v, 1);
            if (!num) return std::unexpected(num.error());
            if (a == "--heartbeat-ms") o.heartbeat_ms = *num;
            else if (a == "--timeout-ms") o.timeout_ms = *num;
            else if (a == "--backoff-ms") o.backoff_ms = *num;
            else o.max_backoff_ms = *num;
        } else if (a == "--message") {
            auto v = need(a);
            if (!v) return std::unexpected(v.error());
            o.message = *v;
            o.has_message = true;
        } else if (a == "--seed") {
            auto v = need(a);
            if (!v) return std::unexpected(v.error());
            auto num = parse_long(*v, 0);
            if (!num) return std::unexpected(num.error());
            o.seed = static_cast<unsigned>(*num);
            o.has_seed = true;
        } else {
            return std::unexpected("unknown flag " + a);
        }
    }
    if (o.name.empty()) {
        return std::unexpected("--name is required");
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage();
    }
    const std::string& sub = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());

    if (sub == "clockprobe") {
        return run_clockprobe();
    }
    if (sub == "listen" || sub == "connect") {
        auto opts = parse_common(rest);
        if (!opts) {
            std::println(stderr, "chatterd: error: {}", opts.error());
            usage();
        }
        if (sub == "listen") {
            if (opts->listen_addr.empty()) {
                std::println(stderr, "chatterd: error: listen needs --addr");
                usage();
            }
            return run_listen(*opts);
        }
        if (opts->peer_addr.empty()) {
            std::println(stderr, "chatterd: error: connect needs --peer");
            usage();
        }
        return run_connect(*opts);
    }
    usage();
}
