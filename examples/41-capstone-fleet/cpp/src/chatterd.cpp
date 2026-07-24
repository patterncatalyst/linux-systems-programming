// chatterd.cpp — the book's recurring peer-to-peer chat daemon (ch21-27),
// reduced to what the capstone needs: serve local clients, and (new in this
// chapter) bridge two chatterd instances across hosts so a message sent on
// one node is delivered to clients on the other. The wire frame (proto.hpp)
// is unchanged from every prior chatterd chapter.
//
// Bridging design (identical to go/chatterd.go's): a bridge worker is just an
// ordinary client of the *other* node's server (nick "bridge@<local-node>").
// Concretely, with --peer set on both sides:
//
//   target dials peer, joins as "bridge@target"   (this connection lives in
//                                                   peer's client registry)
//   peer   dials target, joins as "bridge@peer"   (lives in target's registry)
//
// A local MSG is broadcast as DELIVER to every registered connection
// (including a bridge connection dialed in from the other side) exactly like
// any other client — no protocol change. When a bridge *worker* itself
// receives a DELIVER over the connection it dialed, it re-broadcasts locally
// with include_bridges=false, so the message never goes back out over any
// bridge: no ping-pong between two nodes.
//
// C++23: RAII Fd for every socket, a Hub guarded by one mutex plus a
// per-connection write mutex (the direct translation of the Go reference's
// peerConn.wmu), and a poll(2)-with-timeout accept loop (the same
// shutdown-safe shape ex40's fastpath server uses) instead of closing a
// blocked accept() from another thread.
#include "chatterd.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <print>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <opentelemetry/trace/scope.h>
#include <opentelemetry/trace/span.h>

#include "proto.hpp"
#include "util.hpp"

namespace chatterd {

namespace {

namespace trace_api = opentelemetry::trace;

[[nodiscard]] std::string errno_msg() { return std::string(std::strerror(errno)); }

// ---------------------------------------------------------------------------
// RAII fd — same shape as every other example's.
// ---------------------------------------------------------------------------

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
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
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

private:
    int fd_ = -1;
};

// ---------------------------------------------------------------------------
// TCP helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::expected<Fd, std::string> listen_tcp(const std::string& host, int port) {
    Fd sock(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!sock.valid()) {
        return std::unexpected("socket: " + errno_msg());
    }
    int one = 1;
    ::setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return std::unexpected(host + ": not an IPv4 address");
    }
    if (::bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return std::unexpected("bind: " + errno_msg());
    }
    if (::listen(sock.get(), 16) < 0) {
        return std::unexpected("listen: " + errno_msg());
    }
    return sock;
}

// Non-blocking connect + poll(2), the syscall-level equivalent of Go's
// net.DialTimeout.
[[nodiscard]] std::expected<Fd, std::string> connect_tcp(const std::string& host, int port, int timeout_ms) {
    Fd sock(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!sock.valid()) {
        return std::unexpected("socket: " + errno_msg());
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return std::unexpected(host + ": not an IPv4 address");
    }

    const int flags = ::fcntl(sock.get(), F_GETFL, 0);
    ::fcntl(sock.get(), F_SETFL, flags | O_NONBLOCK);

    if (::connect(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
            return std::unexpected(errno_msg());
        }
        pollfd pfd{.fd = sock.get(), .events = POLLOUT, .revents = 0};
        const int pr = ::poll(&pfd, 1, timeout_ms);
        if (pr <= 0) {
            return std::unexpected("i/o timeout");
        }
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        ::getsockopt(sock.get(), SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (soerr != 0) {
            return std::unexpected(std::strerror(soerr));
        }
    }
    ::fcntl(sock.get(), F_SETFL, flags); // restore blocking mode
    return sock;
}

// ---------------------------------------------------------------------------
// Hub — the registry of every connection (local client or bridge worker)
// currently attached to this server.
// ---------------------------------------------------------------------------

struct PeerConn {
    int fd = -1;
    std::string nick;
    bool is_bridge = false;
    std::mutex wmu;
};

class Hub {
public:
    void add(const std::shared_ptr<PeerConn>& c) {
        std::lock_guard lock(mu_);
        clients_.insert(c);
    }

    void remove(const std::shared_ptr<PeerConn>& c) {
        std::lock_guard lock(mu_);
        clients_.erase(c);
    }

    void broadcast_deliver(const std::shared_ptr<PeerConn>& exclude, bool include_bridges, const std::string& nick,
                            const std::string& text) {
        std::vector<std::shared_ptr<PeerConn>> targets;
        {
            std::lock_guard lock(mu_);
            targets.reserve(clients_.size());
            for (const auto& c : clients_) {
                if (c == exclude) {
                    continue;
                }
                if (c->is_bridge && !include_bridges) {
                    continue;
                }
                targets.push_back(c);
            }
        }
        proto::Frame f{proto::kDeliver, proto::deliver_payload(nick, text)};
        for (const auto& c : targets) {
            std::lock_guard wlock(c->wmu);
            (void)proto::write_frame(c->fd, f); // best-effort, matches Go's `_ = c.send(f)`
        }
    }

private:
    std::mutex mu_;
    std::set<std::shared_ptr<PeerConn>> clients_;
};

void handle_client(int fd, Hub& hub, std::string node, telemetry::Handle& tel) {
    auto pc = std::make_shared<PeerConn>();
    pc->fd = fd;

    auto first = proto::read_frame(fd);
    if (!first || first->type != proto::kJoin) {
        ::close(fd);
        return;
    }
    pc->nick.assign(first->payload.begin(), first->payload.end());
    pc->is_bridge = pc->nick.starts_with("bridge@");
    hub.add(pc);

    for (;;) {
        auto f = proto::read_frame(fd);
        if (!f) {
            break;
        }
        if (f->type != proto::kMsg) {
            continue;
        }
        std::string text(f->payload.begin(), f->payload.end());

        auto span = tel.tracer->StartSpan(
            "chatterd.deliver", {{"chat.from", pc->nick},
                                 {"chat.text_len", static_cast<std::int64_t>(text.size())},
                                 {"chat.node", node},
                                 {"chat.from_bridge", pc->is_bridge}});
        trace_api::Scope scope(span);
        hub.broadcast_deliver(pc, true, pc->nick, text);
        span->SetStatus(trace_api::StatusCode::kOk, "");
        if (tel.enabled) {
            char hex[32];
            span->GetContext().trace_id().ToLowerBase16(opentelemetry::nostd::span<char, 32>(hex, 32));
            std::println("chatterd: trace_id={} node={} from={}", std::string_view(hex, 32), node, pc->nick);
            std::fflush(stdout);
        }
        span->End();
    }
    hub.remove(pc);
    ::close(fd);
}

void bridge_worker(std::string peer_addr, std::string local_node, std::string peer_node, Hub& hub,
                    std::atomic<bool>& sig) {
    const auto colon = peer_addr.rfind(':');
    if (colon == std::string::npos) {
        return;
    }
    const std::string host = peer_addr.substr(0, colon);
    const int port = std::atoi(peer_addr.substr(colon + 1).c_str());
    constexpr auto kBackoff = std::chrono::milliseconds(300);

    while (!sig.load(std::memory_order_relaxed)) {
        auto conn = connect_tcp(host, port, 3000);
        if (!conn) {
            std::this_thread::sleep_for(kBackoff);
            continue;
        }
        const std::string bridge_nick = "bridge@" + local_node;
        proto::Frame joinf{proto::kJoin, {bridge_nick.begin(), bridge_nick.end()}};
        if (auto r = proto::write_frame(conn->get(), joinf); !r) {
            std::this_thread::sleep_for(kBackoff);
            continue;
        }
        std::println(stderr, "chatterd: bridge connected peer={} as={}", peer_addr, bridge_nick);
        std::fflush(stderr);

        for (;;) {
            auto f = proto::read_frame(conn->get());
            if (!f) {
                break;
            }
            if (f->type != proto::kDeliver) {
                continue;
            }
            std::string nick;
            std::string text;
            if (!proto::split_deliver(f->payload, nick, text)) {
                continue;
            }
            if (nick.find('@') == std::string::npos) {
                nick += "@" + peer_node;
            }
            hub.broadcast_deliver(nullptr, false, nick, text);
        }
        std::println(stderr, "chatterd: bridge disconnected peer={} (retrying)", peer_addr);
        std::fflush(stderr);
        std::this_thread::sleep_for(kBackoff);
    }
}

} // namespace

int serve(const std::string& host, int port, const std::string& node, const std::string& peer,
          const std::string& peer_node_in, telemetry::Handle& tel) {
    auto lfd = listen_tcp(host, port);
    if (!lfd) {
        std::println(stderr, "chatterd: listen {}:{}: {}", host, port, lfd.error());
        return 1;
    }
    std::println(stderr, "chatterd: listening on {}:{} node={}", host, port, node);
    std::fflush(stderr);

    Hub hub;
    auto& sig = util::install_signal_flag();

    if (!peer.empty()) {
        std::string peer_node = peer_node_in.empty() ? "remote" : peer_node_in;
        std::thread(bridge_worker, peer, node, peer_node, std::ref(hub), std::ref(sig)).detach();
    }

    for (;;) {
        if (sig.load(std::memory_order_relaxed)) {
            std::println(stderr, "chatterd: shutdown");
            return 0;
        }
        pollfd pfd{.fd = lfd->get(), .events = POLLIN, .revents = 0};
        const int pr = ::poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::println(stderr, "chatterd: poll: {}", errno_msg());
            return 1;
        }
        if (pr == 0) {
            continue;
        }
        const int cfd = ::accept4(lfd->get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) {
            continue;
        }
        std::thread(handle_client, cfd, std::ref(hub), node, std::ref(tel)).detach();
    }
}

// ---------------------------------------------------------------------------
// send / listen — minimal test clients used by verify to prove cross-host
// delivery deterministically (this capstone trims ch27's fuller "loadtest").
// ---------------------------------------------------------------------------

int send(const std::string& host, int port, const std::string& nick, const std::string& text, int timeout_ms) {
    auto conn = connect_tcp(host, port, timeout_ms);
    if (!conn) {
        std::println(stderr, "chatterd: send: dial {}:{}: {}", host, port, conn.error());
        return 1;
    }
    proto::Frame joinf{proto::kJoin, {nick.begin(), nick.end()}};
    if (auto r = proto::write_frame(conn->get(), joinf); !r) {
        std::println(stderr, "chatterd: send: join: {}", r.error().message());
        return 1;
    }
    proto::Frame msgf{proto::kMsg, {text.begin(), text.end()}};
    if (auto r = proto::write_frame(conn->get(), msgf); !r) {
        std::println(stderr, "chatterd: send: msg: {}", r.error().message());
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // let the write actually flush before closing
    std::println("chatterd: sent nick={} text={}", nick, text);
    return 0;
}

int listen(const std::string& host, int port, const std::string& nick, int timeout_ms) {
    auto conn = connect_tcp(host, port, 3000);
    if (!conn) {
        std::println(stderr, "chatterd: listen: dial {}:{}: {}", host, port, conn.error());
        return 1;
    }
    proto::Frame joinf{proto::kJoin, {nick.begin(), nick.end()}};
    if (auto r = proto::write_frame(conn->get(), joinf); !r) {
        std::println(stderr, "chatterd: listen: join: {}", r.error().message());
        return 1;
    }

    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(conn->get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    for (;;) {
        auto f = proto::read_frame(conn->get());
        if (!f) {
            std::println(stderr, "chatterd: listen timeout");
            return 1;
        }
        if (f->type != proto::kDeliver) {
            continue;
        }
        std::string from_nick;
        std::string text;
        if (!proto::split_deliver(f->payload, from_nick, text)) {
            continue;
        }
        std::println("chatterd: received from={} text={}", from_nick, text);
        return 0;
    }
}

} // namespace chatterd
