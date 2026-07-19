// chatterd v4 (C++23) — a peer-to-peer chat daemon.
//
// The connection-handling engine is selectable:
//   * thread : one std::jthread per client (the ch21 baseline).
//   * async  : a single-threaded C++20 coroutine reactor over epoll — each
//              connection is a coroutine that `co_await`s socket readability.
//              The task/awaitable machinery is hand-rolled here; no external
//              async library is used.
//
// Both engines speak the SAME length-prefixed frame protocol and are
// observably identical: a broadcast reaches every other connected client.
//
// Usage:
//   app serve [--engine thread|async] [--host H] [--port P]
//   app loadtest [--host H] [--port P] [--clients N]
//
// Wire format (canonical chatterd chat frame, v1..v4):
//   [ magic 0x43 0x48 ][ version 0x01 ][ type u8 ][ length u16 be ][ payload ]
//   type: JOIN=1, MSG=2, DELIVER=3, WELCOME=4, PING=5.
//   The client's first frame is JOIN (payload = its nick). Every later client
//   frame is MSG (payload = message text). For a MSG with text T from the
//   client whose nick is N, the server sends every OTHER client a DELIVER
//   frame whose payload is N + 0x00 (NUL) + T. This chapter's engines use
//   only JOIN/MSG/DELIVER — WELCOME and PING are other versions' additions to
//   the same frame and are simply never sent here.

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <expected>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

// --------------------------------------------------------------------------
// Shutdown plumbing: a signal sets the flag and kicks the wake eventfd so a
// blocked epoll_wait/poll returns promptly.
// --------------------------------------------------------------------------
std::atomic<bool> g_stop{false};
volatile std::sig_atomic_t g_wake_fd = -1;

extern "C" void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
    if (g_wake_fd >= 0) {
        std::uint64_t one = 1;
        ssize_t r = ::write(g_wake_fd, &one, sizeof one);
        (void)r; // best effort; async-signal-safe
    }
}

void install_signals() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: interrupt blocking syscalls
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    struct sigaction ign{};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, nullptr); // never die on a write to a gone peer
}

// --------------------------------------------------------------------------
// RAII owner for a file descriptor — closes on scope exit, move-only.
// --------------------------------------------------------------------------
class Fd {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
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

    [[nodiscard]] int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

// --------------------------------------------------------------------------
// Frame helpers — canonical chatterd chat frame (see file header).
// --------------------------------------------------------------------------
constexpr int kMaxBody = 65535;
constexpr std::size_t kHeaderLen = 6;
constexpr std::uint8_t kMagic0 = 0x43;
constexpr std::uint8_t kMagic1 = 0x48;
constexpr std::uint8_t kVersion = 0x01;
constexpr std::uint8_t kTypeJoin = 1;
constexpr std::uint8_t kTypeMsg = 2;
constexpr std::uint8_t kTypeDeliver = 3;

struct Frame {
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
    std::uint16_t len = static_cast<std::uint16_t>(body.size());
    out.push_back(static_cast<char>((len >> 8) & 0xff));
    out.push_back(static_cast<char>(len & 0xff));
    out.append(body);
    return out;
}

// Write every byte of `data` to fd, tolerating short writes / EAGAIN / EINTR.
void send_all(int fd, std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p{fd, POLLOUT, 0};
            ::poll(&p, 1, 1000);
            continue;
        }
        return; // peer gone or hard error: drop this recipient
    }
}

// Blocking frame reader: pull bytes into `buf` until one whole frame is
// available, hand its type+body back, and consume it. false => EOF / error.
bool read_frame_blocking(int fd, std::string& buf, Frame& out) {
    for (;;) {
        if (buf.size() >= kHeaderLen) {
            std::uint8_t type = static_cast<std::uint8_t>(buf[3]);
            std::uint16_t len = (static_cast<std::uint8_t>(buf[4]) << 8) |
                                static_cast<std::uint8_t>(buf[5]);
            if (buf.size() >= kHeaderLen + len) {
                out.type = type;
                out.body.assign(buf, kHeaderLen, len);
                buf.erase(0, kHeaderLen + len);
                return true;
            }
        }
        char tmp[4096];
        ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n > 0) {
            buf.append(tmp, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

// --------------------------------------------------------------------------
// Shared client registry used by every engine.
// --------------------------------------------------------------------------
class Server {
public:
    void add(int fd, std::string nick) {
        std::scoped_lock lk(mu_);
        clients_.push_back({fd, std::move(nick)});
    }
    void remove(int fd) {
        std::scoped_lock lk(mu_);
        std::erase_if(clients_, [&](const Client& c) { return c.fd == fd; });
    }
    // Deliver a DELIVER frame (payload = nick + NUL + text) to every client
    // except the sender.
    void broadcast(int from_fd, std::string_view nick, std::string_view text) {
        std::string payload;
        payload.reserve(nick.size() + 1 + text.size());
        payload.append(nick);
        payload.push_back('\0');
        payload.append(text);
        std::string frame = encode_frame(kTypeDeliver, payload);
        std::scoped_lock lk(mu_);
        for (const Client& c : clients_) {
            if (c.fd != from_fd) {
                send_all(c.fd, frame);
            }
        }
    }
    // Wake every blocked client read so thread workers can retire.
    void shutdown_all() {
        std::scoped_lock lk(mu_);
        for (const Client& c : clients_) {
            ::shutdown(c.fd, SHUT_RDWR);
        }
    }

private:
    struct Client {
        int fd;
        std::string nick;
    };
    std::mutex mu_;
    std::vector<Client> clients_;
};

// --------------------------------------------------------------------------
// Networking setup.
// --------------------------------------------------------------------------
std::expected<Fd, std::string> make_listener(const std::string& host, std::uint16_t port,
                                              bool nonblock) {
    int type = SOCK_STREAM | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0);
    Fd s{::socket(AF_INET, type, 0)};
    if (!s) {
        return std::unexpected(std::string("socket: ") + std::strerror(errno));
    }
    int one = 1;
    ::setsockopt(s.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return std::unexpected("inet_pton: bad host " + host);
    }
    if (::bind(s.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        return std::unexpected(std::string("bind: ") + std::strerror(errno));
    }
    if (::listen(s.get(), 128) < 0) {
        return std::unexpected(std::string("listen: ") + std::strerror(errno));
    }
    return s;
}

std::expected<Fd, std::string> connect_to(const std::string& host, std::uint16_t port) {
    Fd s{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!s) {
        return std::unexpected(std::string("socket: ") + std::strerror(errno));
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return std::unexpected("inet_pton: bad host " + host);
    }
    if (::connect(s.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        return std::unexpected(std::string("connect: ") + std::strerror(errno));
    }
    return s;
}

// ==========================================================================
// thread engine — one jthread per client.
// ==========================================================================
void serve_client_blocking(Server& server, Fd conn) {
    int fd = conn.get();
    std::string buf, nick;
    Frame f;
    bool have_nick = false;
    while (read_frame_blocking(fd, buf, f)) {
        if (!have_nick) {
            // First frame is JOIN: its payload is the nick.
            nick = f.body;
            have_nick = true;
            server.add(fd, nick);
        } else {
            // Every later frame is MSG: its payload is the message text.
            server.broadcast(fd, nick, f.body);
        }
    }
    if (have_nick) {
        server.remove(fd);
    }
} // Fd destructor closes the socket

std::expected<void, std::string> run_thread(const std::string& host, std::uint16_t port) {
    auto lfd = make_listener(host, port, /*nonblock=*/true);
    if (!lfd) {
        return std::unexpected(lfd.error());
    }
    Fd listen = std::move(*lfd);
    Fd wake{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    if (!wake) {
        return std::unexpected(std::string("eventfd: ") + std::strerror(errno));
    }
    g_wake_fd = wake.get();

    std::println(stderr, "chatterd: listening on {}:{} engine=thread", host, port);

    Server server;
    std::vector<std::jthread> workers;
    while (!g_stop.load(std::memory_order_relaxed)) {
        struct pollfd pfds[2] = {{listen.get(), POLLIN, 0}, {wake.get(), POLLIN, 0}};
        int pr = ::poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pfds[1].revents & POLLIN) {
            break; // signalled
        }
        if (pfds[0].revents & POLLIN) {
            for (;;) {
                int cfd = ::accept4(listen.get(), nullptr, nullptr, SOCK_CLOEXEC);
                if (cfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
                workers.emplace_back(
                    [&server, f = Fd{cfd}]() mutable { serve_client_blocking(server, std::move(f)); });
            }
        }
    }
    server.shutdown_all(); // unblock client reads
    return {};             // workers joined by ~jthread
}

// ==========================================================================
// async engine — hand-rolled coroutine reactor over epoll.
// ==========================================================================

// A fire-and-forget coroutine: starts eagerly, destroys its own frame when it
// finishes. Used for the top-level acceptor and per-connection coroutines.
struct Detached {
    struct promise_type {
        Detached get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { std::terminate(); }
    };
};

// A value-returning coroutine that can be co_awaited. Lazy start + symmetric
// transfer back to its awaiter on completion.
template <class T>
class Task {
public:
    struct promise_type {
        std::coroutine_handle<> cont_{};
        T value_{};
        Task get_return_object() noexcept {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        struct Final {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept {
                auto c = h.promise().cont_;
                return c ? c : std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        Final final_suspend() noexcept { return {}; }
        void return_value(T v) noexcept { value_ = std::move(v); }
        void unhandled_exception() { std::terminate(); }
    };
    using handle_t = std::coroutine_handle<promise_type>;

    explicit Task(handle_t h) noexcept : h_(h) {}
    Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task& operator=(Task&&) = delete;
    Task(const Task&) = delete;
    ~Task() {
        if (h_) {
            h_.destroy();
        }
    }

    bool await_ready() noexcept { return false; }
    handle_t await_suspend(std::coroutine_handle<> caller) noexcept {
        h_.promise().cont_ = caller;
        return h_; // symmetric transfer: begin the task
    }
    T await_resume() noexcept { return std::move(h_.promise().value_); }

private:
    handle_t h_;
};

// The reactor: maps a waited-on fd to the coroutine parked on its readability.
class Reactor {
public:
    Reactor(Fd ep, int wake_fd) : ep_(std::move(ep)), wake_fd_(wake_fd) {
        epoll_event ev{};
        ev.events = EPOLLIN; // persistent: the wake fd is never rearmed
        ev.data.fd = wake_fd_;
        ::epoll_ctl(ep_.get(), EPOLL_CTL_ADD, wake_fd_, &ev);
    }

    void wait_readable(int fd, std::coroutine_handle<> h) {
        waiters_[fd] = h;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLONESHOT;
        ev.data.fd = fd;
        if (armed_.contains(fd)) {
            ::epoll_ctl(ep_.get(), EPOLL_CTL_MOD, fd, &ev);
        } else {
            ::epoll_ctl(ep_.get(), EPOLL_CTL_ADD, fd, &ev);
            armed_.insert(fd);
        }
    }

    void forget(int fd) {
        if (armed_.contains(fd)) {
            ::epoll_ctl(ep_.get(), EPOLL_CTL_DEL, fd, nullptr);
            armed_.erase(fd);
        }
        waiters_.erase(fd);
    }

    void run() {
        std::array<epoll_event, 64> evs{};
        while (!g_stop.load(std::memory_order_relaxed)) {
            int n = ::epoll_wait(ep_.get(), evs.data(), static_cast<int>(evs.size()), -1);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            for (int i = 0; i < n; ++i) {
                int fd = evs[i].data.fd;
                if (fd == wake_fd_) {
                    continue; // loop guard picks up g_stop
                }
                auto it = waiters_.find(fd);
                if (it == waiters_.end()) {
                    continue;
                }
                auto h = it->second;
                waiters_.erase(it);
                h.resume();
            }
        }
    }

private:
    Fd ep_;
    int wake_fd_;
    std::unordered_map<int, std::coroutine_handle<>> waiters_;
    std::unordered_set<int> armed_;
};

// Suspend the current coroutine until `fd` is readable.
struct ReadAwaitable {
    Reactor& reactor;
    int fd;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const { reactor.wait_readable(fd, h); }
    void await_resume() const noexcept {}
};

// Pull at least one more byte into `buf`. false => EOF / error.
Task<bool> fill(Reactor& reactor, int fd, std::string& buf) {
    for (;;) {
        char tmp[4096];
        ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n > 0) {
            buf.append(tmp, static_cast<std::size_t>(n));
            co_return true;
        }
        if (n == 0) {
            co_return false; // peer closed
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            co_await ReadAwaitable{reactor, fd};
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        co_return false;
    }
}

Detached connection(Reactor& reactor, Server& server, Fd conn) {
    int fd = conn.get();
    std::string buf, nick;
    bool have_nick = false;
    bool alive = true;
    while (alive) {
        while (alive && buf.size() < kHeaderLen) {
            if (!co_await fill(reactor, fd, buf)) {
                alive = false;
            }
        }
        if (!alive) {
            break;
        }
        std::uint16_t len = (static_cast<std::uint8_t>(buf[4]) << 8) |
                            static_cast<std::uint8_t>(buf[5]);
        while (alive && buf.size() < kHeaderLen + len) {
            if (!co_await fill(reactor, fd, buf)) {
                alive = false;
            }
        }
        if (!alive) {
            break;
        }
        std::string body = buf.substr(kHeaderLen, len);
        buf.erase(0, kHeaderLen + len);
        if (!have_nick) {
            // First frame is JOIN: its payload is the nick.
            nick = body;
            have_nick = true;
            server.add(fd, nick);
        } else {
            // Every later frame is MSG: its payload is the message text.
            server.broadcast(fd, nick, body);
        }
    }
    reactor.forget(fd);
    if (have_nick) {
        server.remove(fd);
    }
    co_return; // Fd destructor closes the socket
}

Detached acceptor(Reactor& reactor, Server& server, int listen_fd) {
    for (;;) {
        co_await ReadAwaitable{reactor, listen_fd};
        for (;;) {
            int cfd = ::accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (cfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            connection(reactor, server, Fd{cfd}); // detached; self-owning
        }
    }
}

std::expected<void, std::string> run_async(const std::string& host, std::uint16_t port) {
    auto lfd = make_listener(host, port, /*nonblock=*/true);
    if (!lfd) {
        return std::unexpected(lfd.error());
    }
    Fd listen = std::move(*lfd);
    Fd wake{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    if (!wake) {
        return std::unexpected(std::string("eventfd: ") + std::strerror(errno));
    }
    g_wake_fd = wake.get();
    Fd ep{::epoll_create1(EPOLL_CLOEXEC)};
    if (!ep) {
        return std::unexpected(std::string("epoll_create1: ") + std::strerror(errno));
    }

    std::println(stderr, "chatterd: listening on {}:{} engine=async", host, port);

    Reactor reactor{std::move(ep), wake.get()};
    Server server;
    acceptor(reactor, server, listen.get()); // detached
    reactor.run();
    return {};
}

// ==========================================================================
// loadtest client — drives the broadcast assertion.
// ==========================================================================
int run_loadtest(const std::string& host, std::uint16_t port, int nclients) {
    std::vector<Fd> receivers;
    std::vector<std::string> bufs(static_cast<std::size_t>(nclients));
    receivers.reserve(static_cast<std::size_t>(nclients));

    for (int i = 0; i < nclients; ++i) {
        auto c = connect_to(host, port);
        if (!c) {
            std::println(stderr, "loadtest: connect failed: {}", c.error());
            return 1;
        }
        struct timeval tv{5, 0};
        ::setsockopt(c->get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        std::string hello = encode_frame(kTypeJoin, "r" + std::to_string(i));
        send_all(c->get(), hello);
        receivers.push_back(std::move(*c));
    }

    auto sender = connect_to(host, port);
    if (!sender) {
        std::println(stderr, "loadtest: sender connect failed: {}", sender.error());
        return 1;
    }
    send_all(sender->get(), encode_frame(kTypeJoin, "sender")); // JOIN

    // Let the server register every client before the broadcast goes out.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    send_all(sender->get(), encode_frame(kTypeMsg, "hello world"));

    const std::string want_nick = "sender";
    const std::string want_text = "hello world";
    int delivered = 0;
    for (int i = 0; i < nclients; ++i) {
        Frame f;
        bool ok = read_frame_blocking(receivers[i].get(), bufs[i], f) && f.type == kTypeDeliver;
        if (ok) {
            auto nul = f.body.find('\0');
            ok = nul != std::string::npos && f.body.substr(0, nul) == want_nick &&
                 f.body.substr(nul + 1) == want_text;
        }
        if (ok) {
            ++delivered;
        } else {
            std::println(stderr, "loadtest: client r{} missed the broadcast", i);
        }
    }
    std::println("loadtest: delivered {}/{}", delivered, nclients);
    return delivered == nclients ? 0 : 1;
}

// --------------------------------------------------------------------------
// CLI.
// --------------------------------------------------------------------------
void usage() {
    std::println(stderr,
                 "usage:\n"
                 "  app serve [--engine thread|async] [--host H] [--port P]\n"
                 "  app loadtest [--host H] [--port P] [--clients N]");
}

struct Opts {
    std::string engine = "async";
    std::string host = "127.0.0.1";
    std::uint16_t port = 47100;
    int clients = 20;
};

// Returns false on a malformed option.
bool parse_opts(int argc, char** argv, int start, Opts& o) {
    for (int i = start; i < argc; ++i) {
        std::string_view a = argv[i];
        const char* v = nullptr;
        auto next = [&]() -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            v = argv[++i];
            return true;
        };
        if (a == "--engine" && next()) {
            o.engine = v;
        } else if (a == "--host" && next()) {
            o.host = v;
        } else if (a == "--port" && next()) {
            o.port = static_cast<std::uint16_t>(std::atoi(v));
        } else if (a == "--clients" && next()) {
            o.clients = std::atoi(v);
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    install_signals();
    if (argc < 2) {
        usage();
        return 2;
    }
    std::string cmd = argv[1];
    Opts o;
    if (!parse_opts(argc, argv, 2, o)) {
        usage();
        return 2;
    }

    if (cmd == "serve") {
        std::expected<void, std::string> r;
        if (o.engine == "thread") {
            r = run_thread(o.host, o.port);
        } else if (o.engine == "async") {
            r = run_async(o.host, o.port);
        } else {
            std::println(stderr, "chatterd: unknown engine '{}' (want thread|async)", o.engine);
            return 2;
        }
        if (!r) {
            std::println(stderr, "chatterd: {}", r.error());
            return 1;
        }
        std::println(stderr, "chatterd: shutdown");
        return 0;
    }
    if (cmd == "loadtest") {
        if (o.clients < 1 || o.clients > kMaxBody) {
            std::println(stderr, "loadtest: --clients out of range");
            return 2;
        }
        return run_loadtest(o.host, o.port, o.clients);
    }
    usage();
    return 2;
}
