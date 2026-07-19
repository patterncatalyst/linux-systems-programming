// chatterd v1 — a peer-to-peer chat daemon growing chapter by chapter.
//
// ch21 introduced the length-prefixed frame protocol and a threaded server
// (one thread per connection). ch22 — "scaling the server" — keeps that
// threaded engine working and adds a single-threaded epoll engine: one thread,
// many connections, nonblocking accept plus a per-connection read/write state
// machine with an outbound queue and real backpressure (partial writes are
// carried across epoll_wait iterations, EPOLLOUT drives the flush).
//
// Wire format (identical across cpp/go/rust and every version), big-endian:
//
//   offset 0  2  magic   'C' 'H'  (0x43 0x48)
//   offset 2  1  version 0x01
//   offset 3  1  type
//   offset 4  2  length  u16 payload byte count
//   offset 6  N  payload
//
//   type 0x01 JOIN     c->s  payload = nickname (UTF-8)
//   type 0x02 MSG      c->s  payload = message text (UTF-8)
//   type 0x03 DELIVER  s->c  payload = nick + 0x00 (NUL) + text
//   type 0x04 WELCOME  s->c  payload = empty (sent on accept)
//
// The server delivers every MSG to every connected client, including the
// originator, so a client confirms acceptance by seeing its own line echoed.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

[[nodiscard]] std::error_code last_error() {
    return {errno, std::system_category()};
}

// ---------------------------------------------------------------------------
// RAII file descriptor — the only owner of every fd/socket in this program.
// ---------------------------------------------------------------------------

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_{fd} {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }

    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = -1;
    }
    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_ = -1;
};

[[nodiscard]] std::expected<Fd, std::error_code> checked(int fd) {
    if (fd < 0) {
        return std::unexpected(last_error());
    }
    return Fd{fd};
}

// ---------------------------------------------------------------------------
// Frame protocol
// ---------------------------------------------------------------------------

constexpr std::uint8_t kVersion = 0x01;
enum class Type : std::uint8_t { join = 0x01, msg = 0x02, deliver = 0x03, welcome = 0x04 };

struct Frame {
    std::uint8_t type;
    std::string payload;
};

[[nodiscard]] std::string encode(Type t, std::string_view payload) {
    std::string f;
    f.reserve(6 + payload.size());
    f.push_back('C');
    f.push_back('H');
    f.push_back(static_cast<char>(kVersion));
    f.push_back(static_cast<char>(std::to_underlying(t)));
    f.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
    f.push_back(static_cast<char>(payload.size() & 0xff));
    f.append(payload);
    return f;
}

[[nodiscard]] std::string deliver_payload(std::string_view nick, std::string_view text) {
    std::string p;
    p.reserve(nick.size() + 1 + text.size());
    p.append(nick);
    p.push_back('\0');
    p.append(text);
    return p;
}

// Pull one complete frame off the front of buf, consuming its bytes. Returns
// nullopt when buf holds only a partial frame (need more bytes).
[[nodiscard]] std::optional<Frame> take_frame(std::string& buf) {
    if (buf.size() < 6) {
        return std::nullopt;
    }
    const auto* b = reinterpret_cast<const unsigned char*>(buf.data());
    if (b[0] != 'C' || b[1] != 'H' || b[2] != kVersion) {
        buf.clear(); // desync on a foreign speaker; drop the stream
        return std::nullopt;
    }
    const std::size_t len = (static_cast<std::size_t>(b[4]) << 8) | b[5];
    if (buf.size() < 6 + len) {
        return std::nullopt;
    }
    Frame fr{b[3], buf.substr(6, len)};
    buf.erase(0, 6 + len);
    return fr;
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------

[[nodiscard]] std::expected<std::pair<Fd, std::uint16_t>, std::error_code>
make_listener(std::uint16_t port, bool nonblock) {
    const int type = SOCK_STREAM | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0);
    auto s = checked(::socket(AF_INET, type, 0));
    if (!s) {
        return std::unexpected(s.error());
    }
    int one = 1;
    ::setsockopt(s->get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(s->get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        return std::unexpected(last_error());
    }
    if (::listen(s->get(), 128) != 0) {
        return std::unexpected(last_error());
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof bound;
    if (::getsockname(s->get(), reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
        return std::unexpected(last_error());
    }
    return std::pair{std::move(*s), ntohs(bound.sin_port)};
}

[[nodiscard]] std::expected<Fd, std::error_code> make_signalfd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
        return std::unexpected(last_error());
    }
    return checked(::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
}

// Blocking write of every byte; returns false if the peer went away.
[[nodiscard]] bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n > 0) {
            data.remove_prefix(static_cast<std::size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Threaded engine — one thread per connection, a shared broadcast registry.
// ---------------------------------------------------------------------------

struct Conn {
    Fd fd;
    std::mutex wmu;         // serialize writes to this socket
    std::string nick = "?"; // set by the JOIN frame
};

class Registry {
public:
    void add(std::uint64_t id, std::shared_ptr<Conn> c) {
        std::scoped_lock lk{mu_};
        conns_.emplace(id, std::move(c));
        const auto n = static_cast<int>(conns_.size());
        if (n > peak_.load(std::memory_order_relaxed)) {
            peak_.store(n, std::memory_order_relaxed);
        }
    }
    void remove(std::uint64_t id) {
        std::scoped_lock lk{mu_};
        conns_.erase(id);
    }
    void broadcast(std::string_view frame) {
        std::vector<std::shared_ptr<Conn>> snap;
        {
            std::scoped_lock lk{mu_};
            snap.reserve(conns_.size());
            for (auto& [_, c] : conns_) {
                snap.push_back(c);
            }
        }
        for (auto& c : snap) {
            std::scoped_lock wl{c->wmu};
            (void)write_all(c->fd.get(), frame); // a dead peer is reaped by its reader
        }
    }
    void shutdown_all() {
        std::scoped_lock lk{mu_};
        for (auto& [_, c] : conns_) {
            ::shutdown(c->fd.get(), SHUT_RDWR); // wake the reader blocked in recv()
        }
    }
    [[nodiscard]] int peak() const { return peak_.load(std::memory_order_relaxed); }

private:
    std::mutex mu_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Conn>> conns_;
    std::atomic<int> peak_{0};
};

void serve_conn(std::stop_token stop, std::shared_ptr<Conn> conn, std::uint64_t id,
                Registry& reg, std::atomic<std::uint64_t>& messages) {
    (void)write_all(conn->fd.get(), encode(Type::welcome, ""));
    std::string inbuf;
    std::array<char, 4096> buf{};
    while (!stop.stop_requested()) {
        const ssize_t n = ::recv(conn->fd.get(), buf.data(), buf.size(), 0);
        if (n <= 0) {
            break; // EOF, error, or shutdown() at teardown
        }
        inbuf.append(buf.data(), static_cast<std::size_t>(n));
        while (auto fr = take_frame(inbuf)) {
            if (fr->type == std::to_underlying(Type::join)) {
                std::scoped_lock wl{conn->wmu};
                conn->nick = fr->payload;
            } else if (fr->type == std::to_underlying(Type::msg)) {
                std::string nick;
                {
                    std::scoped_lock wl{conn->wmu};
                    nick = conn->nick;
                }
                messages.fetch_add(1, std::memory_order_relaxed);
                reg.broadcast(encode(Type::deliver, deliver_payload(nick, fr->payload)));
            }
        }
    }
    reg.remove(id);
}

int serve_threaded(std::uint16_t port) {
    auto listener = make_listener(port, /*nonblock=*/false);
    if (!listener) {
        std::println(stderr, "chatterd: listen: {}", listener.error().message());
        return 1;
    }
    auto& [lfd, bound] = *listener;
    auto sigfd = make_signalfd();
    if (!sigfd) {
        std::println(stderr, "chatterd: signalfd: {}", sigfd.error().message());
        return 1;
    }
    std::println(stderr, "chatterd: serving engine=threaded on 127.0.0.1:{}", bound);

    Registry reg;
    std::atomic<std::uint64_t> messages{0};
    std::vector<std::jthread> workers;
    std::uint64_t next_id = 0;

    for (;;) {
        std::array<pollfd, 2> pfds{
            pollfd{.fd = lfd.get(), .events = POLLIN, .revents = 0},
            pollfd{.fd = sigfd->get(), .events = POLLIN, .revents = 0},
        };
        if (::poll(pfds.data(), pfds.size(), -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::println(stderr, "chatterd: poll: {}", last_error().message());
            return 1;
        }
        if ((pfds[1].revents & POLLIN) != 0) {
            break; // SIGINT/SIGTERM
        }
        if ((pfds[0].revents & POLLIN) != 0) {
            auto client = checked(::accept4(lfd.get(), nullptr, nullptr, SOCK_CLOEXEC));
            if (!client) {
                continue;
            }
            auto conn = std::make_shared<Conn>();
            conn->fd = std::move(*client);
            const auto id = next_id++;
            reg.add(id, conn);
            workers.emplace_back([conn, id, &reg, &messages](std::stop_token st) {
                serve_conn(std::move(st), conn, id, reg, messages);
            });
        }
    }

    lfd.reset();
    reg.shutdown_all();
    workers.clear(); // jthread destructors request stop and join every worker
    std::println(stderr, "chatterd: stopped engine=threaded messages={} peak_conns={}",
                 messages.load(), reg.peak());
    return 0;
}

// ---------------------------------------------------------------------------
// Epoll engine — one thread, nonblocking, per-connection state machines.
// ---------------------------------------------------------------------------

struct EConn {
    Fd fd;
    std::string nick = "?";
    std::string inbuf;
    std::string outbuf; // pending outbound bytes (the backpressure queue)
    bool want_write = false;
    bool dead = false;
};

class EpollServer {
public:
    static constexpr std::uint64_t kListen = 0;
    static constexpr std::uint64_t kSignal = 1;

    EpollServer(Fd epoll, Fd listener) : epoll_{std::move(epoll)}, listener_{std::move(listener)} {}

    [[nodiscard]] std::expected<void, std::error_code> register_fd(int fd, std::uint64_t tok,
                                                                   std::uint32_t ev) {
        epoll_event e{.events = ev, .data = {.u64 = tok}};
        if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &e) != 0) {
            return std::unexpected(last_error());
        }
        return {};
    }
    void set_sigfd(Fd sig) { sigfd_ = std::move(sig); }

    int run() {
        std::array<epoll_event, 64> events{};
        for (;;) {
            const int n = ::epoll_wait(epoll_.get(), events.data(), events.size(), -1);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::println(stderr, "chatterd: epoll_wait: {}", last_error().message());
                return 1;
            }
            for (int i = 0; i < n; ++i) {
                const auto tok = events[i].data.u64;
                if (tok == kListen) {
                    accept_ready();
                } else if (tok == kSignal) {
                    reap_closed();
                    std::println(stderr,
                                 "chatterd: stopped engine=epoll messages={} peak_conns={}",
                                 messages_, peak_);
                    return 0;
                } else {
                    if (!conns_.contains(tok)) {
                        continue; // reaped earlier in this batch
                    }
                    if ((events[i].events & (EPOLLHUP | EPOLLERR)) != 0) {
                        conns_.at(tok).dead = true;
                    }
                    if ((events[i].events & EPOLLIN) != 0) {
                        on_readable(tok);
                    }
                    if (conns_.contains(tok) && (events[i].events & EPOLLOUT) != 0) {
                        flush(tok);
                    }
                }
            }
            reap_closed();
        }
    }

private:
    void accept_ready() {
        for (;;) {
            const int cfd = ::accept4(listener_.get(), nullptr, nullptr,
                                      SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (cfd < 0) {
                return; // EAGAIN: backlog drained
            }
            const auto tok = next_tok_++;
            EConn conn;
            conn.fd = Fd{cfd};
            epoll_event e{.events = EPOLLIN, .data = {.u64 = tok}};
            if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, cfd, &e) != 0) {
                continue; // conn.fd closes on scope exit
            }
            conns_.emplace(tok, std::move(conn));
            if (static_cast<int>(conns_.size()) > peak_) {
                peak_ = static_cast<int>(conns_.size());
            }
            queue(tok, encode(Type::welcome, ""));
        }
    }

    void on_readable(std::uint64_t tok) {
        auto& conn = conns_.at(tok);
        std::array<char, 4096> buf{};
        for (;;) {
            const ssize_t n = ::recv(conn.fd.get(), buf.data(), buf.size(), 0);
            if (n > 0) {
                conn.inbuf.append(buf.data(), static_cast<std::size_t>(n));
            } else if (n == 0) {
                conn.dead = true;
                return;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else if (errno == EINTR) {
                continue;
            } else {
                conn.dead = true;
                return;
            }
        }
        while (auto fr = take_frame(conn.inbuf)) {
            if (fr->type == std::to_underlying(Type::join)) {
                conn.nick = fr->payload;
            } else if (fr->type == std::to_underlying(Type::msg)) {
                ++messages_;
                broadcast(encode(Type::deliver, deliver_payload(conn.nick, fr->payload)));
            }
        }
    }

    void broadcast(const std::string& frame) {
        for (auto& [tok, _] : conns_) {
            queue(tok, frame);
        }
    }

    // Append to the connection's outbound queue and try to drain it now.
    void queue(std::uint64_t tok, const std::string& frame) {
        conns_.at(tok).outbuf.append(frame);
        flush(tok);
    }

    void flush(std::uint64_t tok) {
        auto& conn = conns_.at(tok);
        while (!conn.outbuf.empty()) {
            const ssize_t n = ::send(conn.fd.get(), conn.outbuf.data(), conn.outbuf.size(),
                                     MSG_NOSIGNAL);
            if (n > 0) {
                conn.outbuf.erase(0, static_cast<std::size_t>(n));
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break; // socket buffer full: keep the rest, wait for EPOLLOUT
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                conn.dead = true;
                return;
            }
        }
        // Register/clear EPOLLOUT interest to match the backlog state.
        const bool need = !conn.outbuf.empty();
        if (need != conn.want_write) {
            conn.want_write = need;
            epoll_event e{.events = EPOLLIN | (need ? EPOLLOUT : 0u), .data = {.u64 = tok}};
            ::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, conn.fd.get(), &e);
        }
    }

    void reap_closed() {
        for (auto it = conns_.begin(); it != conns_.end();) {
            if (it->second.dead) {
                ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, it->second.fd.get(), nullptr);
                it = conns_.erase(it);
            } else {
                ++it;
            }
        }
    }

    Fd epoll_;
    Fd listener_;
    Fd sigfd_;
    std::unordered_map<std::uint64_t, EConn> conns_;
    std::uint64_t next_tok_ = 2; // 0/1 reserved for listen/signal
    std::uint64_t messages_ = 0;
    int peak_ = 0;
};

int serve_epoll(std::uint16_t port) {
    auto listener = make_listener(port, /*nonblock=*/true);
    if (!listener) {
        std::println(stderr, "chatterd: listen: {}", listener.error().message());
        return 1;
    }
    auto& [lfd, bound] = *listener;
    auto sigfd = make_signalfd();
    if (!sigfd) {
        std::println(stderr, "chatterd: signalfd: {}", sigfd.error().message());
        return 1;
    }
    auto epoll = checked(::epoll_create1(EPOLL_CLOEXEC));
    if (!epoll) {
        std::println(stderr, "chatterd: epoll_create1: {}", epoll.error().message());
        return 1;
    }

    const int lfd_raw = lfd.get();
    const int sfd_raw = sigfd->get();
    EpollServer server{std::move(*epoll), std::move(lfd)};
    if (auto r = server.register_fd(lfd_raw, EpollServer::kListen, EPOLLIN); !r) {
        std::println(stderr, "chatterd: epoll_ctl: {}", r.error().message());
        return 1;
    }
    if (auto r = server.register_fd(sfd_raw, EpollServer::kSignal, EPOLLIN); !r) {
        std::println(stderr, "chatterd: epoll_ctl: {}", r.error().message());
        return 1;
    }
    server.set_sigfd(std::move(*sigfd));

    std::println(stderr, "chatterd: serving engine=epoll on 127.0.0.1:{}", bound);
    return server.run();
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

[[nodiscard]] std::expected<Fd, std::error_code> connect_to(std::uint16_t port) {
    auto s = checked(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!s) {
        return std::unexpected(s.error());
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(s->get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        return std::unexpected(last_error());
    }
    return std::move(*s);
}

// Blocking read of one frame; nullopt on EOF or SO_RCVTIMEO timeout.
[[nodiscard]] std::optional<Frame> recv_frame(int fd, std::string& inbuf) {
    for (;;) {
        if (auto fr = take_frame(inbuf)) {
            return fr;
        }
        std::array<char, 4096> buf{};
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) {
            return std::nullopt;
        }
        inbuf.append(buf.data(), static_cast<std::size_t>(n));
    }
}

[[nodiscard]] std::pair<std::string, std::string> split_deliver(const std::string& payload) {
    const auto nul = payload.find('\0');
    if (nul == std::string::npos) {
        return {"?", payload};
    }
    return {payload.substr(0, nul), payload.substr(nul + 1)};
}

int cmd_send(std::uint16_t port, const std::string& nick, const std::string& text) {
    auto fd = connect_to(port);
    if (!fd) {
        std::println(stderr, "chatctl: cannot connect to 127.0.0.1:{}: {}", port,
                     fd.error().message());
        return 1;
    }
    if (!write_all(fd->get(), encode(Type::join, nick)) ||
        !write_all(fd->get(), encode(Type::msg, text))) {
        std::println(stderr, "chatctl: send failed");
        return 1;
    }
    std::string inbuf;
    while (auto fr = recv_frame(fd->get(), inbuf)) {
        if (fr->type != std::to_underlying(Type::deliver)) {
            continue; // skip WELCOME
        }
        auto [dn, dt] = split_deliver(fr->payload);
        std::println("{}: {}", dn, dt);
        std::fflush(stdout);
        if (dn == nick && dt == text) {
            return 0; // saw our own message echoed back
        }
    }
    return 0;
}

int cmd_listen(std::uint16_t port, const std::string& nick, int count) {
    auto fd = connect_to(port);
    if (!fd) {
        std::println(stderr, "chatctl: cannot connect to 127.0.0.1:{}: {}", port,
                     fd.error().message());
        return 1;
    }
    timeval tv{.tv_sec = 10, .tv_usec = 0};
    ::setsockopt(fd->get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (!write_all(fd->get(), encode(Type::join, nick))) {
        std::println(stderr, "chatctl: send failed");
        return 1;
    }
    std::string inbuf;
    int got = 0;
    while (got < count) {
        auto fr = recv_frame(fd->get(), inbuf);
        if (!fr) {
            std::println(stderr, "chatctl: timed out after {} of {} messages", got, count);
            return 1;
        }
        if (fr->type != std::to_underlying(Type::deliver)) {
            continue;
        }
        auto [dn, dt] = split_deliver(fr->payload);
        std::println("{}: {}", dn, dt);
        std::fflush(stdout);
        ++got;
    }
    return 0;
}

int cmd_flood(std::uint16_t port, int n, const std::string& text) {
    std::atomic<int> joined{0};
    std::atomic<int> delivered{0};
    std::atomic<bool> go{false};
    std::vector<std::jthread> conns;
    conns.reserve(static_cast<std::size_t>(n));

    for (int k = 0; k < n; ++k) {
        conns.emplace_back([&, k] {
            auto fd = connect_to(port);
            if (!fd) {
                return;
            }
            timeval tv{.tv_sec = 10, .tv_usec = 0};
            ::setsockopt(fd->get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            if (!write_all(fd->get(), encode(Type::join, "flooder" + std::to_string(k)))) {
                return;
            }
            std::string inbuf;
            // Wait for the WELCOME: proof the server has us in its fan-out set.
            for (;;) {
                auto fr = recv_frame(fd->get(), inbuf);
                if (!fr) {
                    return;
                }
                if (fr->type == std::to_underlying(Type::welcome)) {
                    break;
                }
            }
            joined.fetch_add(1, std::memory_order_relaxed);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            if (k == 0) {
                (void)write_all(fd->get(), encode(Type::msg, text));
            }
            // Exactly one broadcast is expected — conn0's message reaches all.
            for (;;) {
                auto fr = recv_frame(fd->get(), inbuf);
                if (!fr) {
                    return;
                }
                if (fr->type == std::to_underlying(Type::deliver)) {
                    delivered.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }

    // Wait for every connection to be welcomed before releasing the sender.
    for (int i = 0; i < 5000 && joined.load() < n; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    go.store(true, std::memory_order_release);
    conns.clear(); // join all

    const int j = joined.load();
    const int d = delivered.load();
    std::println("flood: connected {}", j);
    std::println("flood: delivered {}", d);
    return (j == n && d == n) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

int usage() {
    std::println(stderr, "usage: chatterd <command>");
    std::println(stderr, "  serve --engine threaded|epoll --port P   run the chat daemon");
    std::println(stderr, "  send  --port P NICK TEXT                 join, broadcast, print echoes");
    std::println(stderr, "  listen --port P NICK --count N           join, print N delivered lines");
    std::println(stderr, "  flood --port P N [--text TEXT]           open N conns, broadcast, count");
    return 2;
}

[[nodiscard]] std::optional<std::string> flag(const std::vector<std::string_view>& a,
                                              std::string_view name) {
    for (std::size_t i = 0; i + 1 < a.size(); ++i) {
        if (a[i] == name) {
            return std::string{a[i + 1]};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint16_t> parse_port(const std::optional<std::string>& s) {
    if (!s) {
        return std::nullopt;
    }
    try {
        const int p = std::stoi(*s);
        if (p < 0 || p > 65535) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(p);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args{argv + 1, argv + argc};
    if (args.empty()) {
        return usage();
    }
    const auto cmd = args[0];

    if (cmd == "serve") {
        const auto engine = flag(args, "--engine");
        const auto port = parse_port(flag(args, "--port"));
        if (!engine || !port) {
            return usage();
        }
        if (*engine == "threaded") {
            return serve_threaded(*port);
        }
        if (*engine == "epoll") {
            return serve_epoll(*port);
        }
        return usage();
    }
    if (cmd == "send" && args.size() == 5 && args[1] == "--port") {
        const auto port = parse_port(std::string{args[2]});
        if (!port) {
            return usage();
        }
        return cmd_send(*port, std::string{args[3]}, std::string{args[4]});
    }
    if (cmd == "listen" && args.size() == 6 && args[1] == "--port" && args[4] == "--count") {
        const auto port = parse_port(std::string{args[2]});
        if (!port) {
            return usage();
        }
        try {
            const int c = std::stoi(std::string{args[5]});
            if (c <= 0) {
                return usage();
            }
            return cmd_listen(*port, std::string{args[3]}, c);
        } catch (const std::exception&) {
            return usage();
        }
    }
    if (cmd == "flood" && args.size() >= 4 && args[1] == "--port") {
        const auto port = parse_port(std::string{args[2]});
        if (!port) {
            return usage();
        }
        const std::string text = flag(args, "--text").value_or("flood");
        try {
            const int n = std::stoi(std::string{args[3]});
            if (n <= 0) {
                return usage();
            }
            return cmd_flood(*port, n, text);
        } catch (const std::exception&) {
            return usage();
        }
    }
    return usage();
}
