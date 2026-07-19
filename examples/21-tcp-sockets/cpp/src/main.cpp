// chatterd v0 — a thread-per-connection TCP chat room.
//
// One binary, two subcommands:
//
//   chatterd serve   --port P [--host H]
//       Accept TCP connections; each client runs on its own std::jthread.
//       Every chat frame received is broadcast to all OTHER clients; a join
//       notice is broadcast to ALL clients (including the newcomer). SIGINT
//       closes the listener and exits 0.
//
//   chatterd chatctl --port P --name N [--host H]
//       Connect, announce N with a join frame, then send one frame per line
//       of stdin. A reader jthread prints every received frame as
//       "<name>: <text>".
//
// THE WIRE PROTOCOL (fixed for the whole book, ch21-ch27). Every message is
// the canonical chatterd CHAT FRAME:
//
//     +-------+---------+------+----------------+-------------------+
//     | magic | version | type | length (u16BE) | payload (UTF-8)   |
//     | 2B    | 1B      | 1B   | 2B             | `length` bytes    |
//     +-------+---------+------+----------------+-------------------+
//
// magic is the two bytes 0x43 0x48 ("CH"); version is 0x01. `length` counts
// the payload only (0..65535), never the 6-byte header. This chapter (v0)
// uses three of the five frame types:
//
//   JOIN    (1) payload = name              client -> server, once, at connect
//   MSG     (2) payload = text               client -> server, per stdin line
//   DELIVER (3) payload = name 0x00 text      server -> all clients, a broadcast
//
// WELCOME (4) and PING (5) are reserved for later chapters (ch22, ch24); this
// program never sends them, and a chatctl reader here ignores any frame type
// it doesn't recognize so it stays interoperable with newer servers. The
// server relays a client's MSG as a DELIVER and synthesises join/leave
// DELIVER notices from the reserved sender name "server".
//
// C++23: a RAII Socket owns every fd (no naked close); std::expected carries
// each fallible step; std::jthread + std::stop_token drive the connection
// threads; a std::atomic flag plus a SIGINT-interruptible poll(2) shut the
// listener down.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <iostream>
#include <mutex>
#include <optional>
#include <print>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// The canonical chatterd chat frame header: magic "CH", version 1.
constexpr std::uint8_t kMagic0 = 0x43;
constexpr std::uint8_t kMagic1 = 0x48;
constexpr std::uint8_t kWireVersion = 0x01;
constexpr std::size_t kHeaderSize = 6;  // magic(2) + version(1) + type(1) + length(2)

enum FrameType : std::uint8_t {
    kJoin = 1,
    kMsg = 2,
    kDeliver = 3,
    kWelcome = 4,
    kPing = 5,
};

[[nodiscard]] std::string errno_text() {
    return std::error_code{errno, std::system_category()}.message();
}

template <typename T>
using Result = std::expected<T, std::string>;

// ---------------------------------------------------------------------------
// RAII Socket — the sole owner of every descriptor in this program.
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

// ---------------------------------------------------------------------------
// The canonical chat frame: magic + version + type + u16BE length + payload.
// ---------------------------------------------------------------------------

struct RawFrame {
    std::uint8_t type;
    std::string payload;
};

// Read exactly n bytes; false on EOF or hard error (EINTR is retried).
bool read_full(int fd, void* buf, std::size_t n) {
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t off = 0;
    while (off < n) {
        ssize_t r = ::read(fd, p + off, n - off);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
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

void write_all(int fd, std::string_view text) {
    std::size_t off = 0;
    while (off < text.size()) {
        ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;  // peer gone; nothing sensible to do here
        }
        off += static_cast<std::size_t>(n);
    }
}

std::string build_frame(std::uint8_t type, std::string_view payload) {
    // payload.size() is always <= 65535 for the frames this program builds
    // (names/messages typed on a terminal); the length field is u16BE.
    auto len = static_cast<std::uint16_t>(payload.size());
    std::string frame;
    frame.reserve(kHeaderSize + payload.size());
    frame.push_back(static_cast<char>(kMagic0));
    frame.push_back(static_cast<char>(kMagic1));
    frame.push_back(static_cast<char>(kWireVersion));
    frame.push_back(static_cast<char>(type));
    frame.push_back(static_cast<char>((len >> 8) & 0xff));
    frame.push_back(static_cast<char>(len & 0xff));
    frame.append(payload);
    return frame;
}

std::string build_join(std::string_view name) { return build_frame(kJoin, name); }
std::string build_msg(std::string_view text) { return build_frame(kMsg, text); }
std::string build_deliver(std::string_view name, std::string_view text) {
    std::string payload;
    payload.reserve(name.size() + 1 + text.size());
    payload.append(name);
    payload.push_back('\0');
    payload.append(text);
    return build_frame(kDeliver, payload);
}

std::optional<RawFrame> read_frame(int fd) {
    std::uint8_t hdr[kHeaderSize];
    if (!read_full(fd, hdr, kHeaderSize)) {
        return std::nullopt;
    }
    if (hdr[0] != kMagic0 || hdr[1] != kMagic1 || hdr[2] != kWireVersion) {
        return std::nullopt;  // wrong magic/version: not a chatterd frame
    }
    std::uint8_t type = hdr[3];
    std::uint16_t len = (static_cast<std::uint16_t>(hdr[4]) << 8) | hdr[5];
    std::string payload(len, '\0');
    if (len > 0 && !read_full(fd, payload.data(), len)) {
        return std::nullopt;
    }
    return RawFrame{type, std::move(payload)};
}

// ---------------------------------------------------------------------------
// The hub: every connected client, keyed by id.
// ---------------------------------------------------------------------------

constexpr int kBroadcastAll = -1;

class Hub {
public:
    void add(int id, int fd) {
        std::lock_guard lk(m_);
        clients_.push_back({id, std::string{}, fd});
    }
    void set_name(int id, std::string_view name) {
        std::lock_guard lk(m_);
        if (auto* c = find(id)) {
            c->name = name;
        }
    }
    // Remove a client and return the name it last carried (empty if unnamed).
    std::string remove(int id) {
        std::lock_guard lk(m_);
        for (auto it = clients_.begin(); it != clients_.end(); ++it) {
            if (it->id == id) {
                std::string name = it->name;
                clients_.erase(it);
                return name;
            }
        }
        return {};
    }
    // Write frame to every client; when except >= 0 that id is skipped.
    void broadcast(std::string_view frame, int except) {
        std::lock_guard lk(m_);
        for (const auto& c : clients_) {
            if (c.id != except) {
                write_all(c.fd, frame);
            }
        }
    }

private:
    struct Client {
        int id;
        std::string name;
        int fd;
    };
    Client* find(int id) {
        auto it = std::ranges::find(clients_, id, &Client::id);
        return it == clients_.end() ? nullptr : &*it;
    }

    std::mutex m_;
    std::vector<Client> clients_;
};

// One connection's lifetime. The stop_callback shuts the socket down when the
// jthread is asked to stop, which unblocks the read below during shutdown.
void serve_client(std::stop_token st, Socket conn, int id, Hub& hub) {
    const int fd = conn.get();
    std::stop_callback on_stop(st, [fd] { ::shutdown(fd, SHUT_RDWR); });

    hub.add(id, fd);
    std::string my_name;
    for (;;) {
        auto raw = read_frame(fd);
        if (!raw) {
            break;
        }
        if (raw->type == kJoin) {
            if (raw->payload.empty()) {
                break;  // malformed: JOIN carries no name
            }
            my_name = raw->payload;
            hub.set_name(id, my_name);
            std::println(stderr, "chatterd: {} joined", my_name);
            hub.broadcast(build_deliver("server", std::format("{} joined", my_name)),
                          kBroadcastAll);
        } else if (raw->type == kMsg) {
            if (my_name.empty()) {
                continue;  // MSG before JOIN: nothing sensible to attribute it to
            }
            hub.broadcast(build_deliver(my_name, raw->payload), id);
        }
        // Any other type (DELIVER/WELCOME/PING) is not sent by a client in this
        // chapter; ignore it rather than tearing the connection down.
    }

    std::string name = hub.remove(id);
    if (!name.empty()) {
        std::println(stderr, "chatterd: {} left", name);
        hub.broadcast(build_deliver("server", std::format("{} left", name)), kBroadcastAll);
    }
}

// ---------------------------------------------------------------------------
// serve
// ---------------------------------------------------------------------------

std::atomic<bool> g_stop{false};

extern "C" void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

Result<Socket> listen_tcp(const std::string& host, std::uint16_t port) {
    Socket sock{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!sock.valid()) {
        return std::unexpected(std::format("socket: {}", errno_text()));
    }
    int one = 1;
    ::setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return std::unexpected(std::format("{}: not an IPv4 address", host));
    }
    if (::bind(sock.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        return std::unexpected(std::format("bind {}:{}: {}", host, port, errno_text()));
    }
    if (::listen(sock.get(), 128) < 0) {
        return std::unexpected(std::format("listen: {}", errno_text()));
    }
    return sock;
}

int cmd_serve(const std::string& host, std::uint16_t port) {
    auto lsock = listen_tcp(host, port);
    if (!lsock) {
        std::println(stderr, "chatterd: error: {}", lsock.error());
        return 1;
    }
    std::signal(SIGINT, on_sigint);
    std::println(stderr, "chatterd: listening on {}:{}", host, port);

    Hub hub;
    std::vector<std::jthread> threads;
    int next_id = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        pollfd pfd{lsock->get(), POLLIN, 0};
        int pr = ::poll(&pfd, 1, 200);  // wake at least every 200 ms to re-check g_stop
        if (pr < 0) {
            if (errno == EINTR) {
                continue;  // SIGINT lands here; the loop re-checks g_stop
            }
            std::println(stderr, "chatterd: error: poll: {}", errno_text());
            return 1;
        }
        if (pr == 0 || !(pfd.revents & POLLIN)) {
            continue;
        }
        Socket conn{::accept4(lsock->get(), nullptr, nullptr, SOCK_CLOEXEC)};
        if (!conn.valid()) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            std::println(stderr, "chatterd: error: accept: {}", errno_text());
            return 1;
        }
        int id = ++next_id;
        threads.emplace_back(
            [&hub, id, c = std::move(conn)](std::stop_token st) mutable {
                serve_client(st, std::move(c), id, hub);
            });
    }

    std::println(stderr, "chatterd: shutting down");
    // Returning destroys `threads`: each jthread's destructor requests stop
    // (firing the stop_callback that shuts the socket) and joins. The listener
    // Socket closes last, when lsock goes out of scope.
    for (auto& t : threads) {
        t.request_stop();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// chatctl
// ---------------------------------------------------------------------------

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

int cmd_chatctl(const std::string& host, std::uint16_t port, const std::string& name) {
    auto conn = connect_tcp(host, port);
    if (!conn) {
        std::println(stderr, "chatctl: error: {}", conn.error());
        return 1;
    }
    // Announce ourselves: a JOIN frame's payload is just our name.
    write_all(conn->get(), build_join(name));

    std::jthread reader([fd = conn->get()](std::stop_token) {
        for (;;) {
            auto raw = read_frame(fd);
            if (!raw) {
                break;
            }
            if (raw->type != kDeliver) {
                continue;  // WELCOME/PING/etc. are for later chapters; ignore them
            }
            auto nul = raw->payload.find('\0');
            if (nul == std::string::npos) {
                continue;  // malformed DELIVER; skip rather than crash
            }
            std::println("{}: {}", raw->payload.substr(0, nul), raw->payload.substr(nul + 1));
            std::fflush(stdout);  // stdout is fully buffered to a pipe/file
        }
    });

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;  // never emit an empty-text frame; that would read as a join
        }
        write_all(conn->get(), build_msg(line));
    }
    // stdin EOF: tear the socket down so the reader sees EOF and joins.
    ::shutdown(conn->get(), SHUT_RDWR);
    return 0;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

int usage() {
    std::println(stderr, "usage: chatterd serve --port PORT [--host HOST]");
    std::println(stderr, "       chatterd chatctl --port PORT --name NAME [--host HOST]");
    return 2;
}

std::optional<std::uint16_t> parse_port(const std::string& s) {
    try {
        std::size_t pos = 0;
        int v = std::stoi(s, &pos);
        if (pos != s.size() || v < 1 || v > 65535) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        return usage();
    }

    const std::string sub = args[0];
    if (sub != "serve" && sub != "chatctl") {
        return usage();
    }

    std::string host = "127.0.0.1";
    std::string port_str;
    std::string name;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--port" && i + 1 < args.size()) {
            port_str = args[++i];
        } else if (args[i] == "--host" && i + 1 < args.size()) {
            host = args[++i];
        } else if (args[i] == "--name" && i + 1 < args.size()) {
            name = args[++i];
        } else {
            return usage();
        }
    }
    if (port_str.empty()) {
        return usage();
    }
    auto port = parse_port(port_str);
    if (!port) {
        return usage();
    }

    if (sub == "serve") {
        return cmd_serve(host, *port);
    }
    if (name.empty()) {
        return usage();
    }
    return cmd_chatctl(host, *port, name);
}
