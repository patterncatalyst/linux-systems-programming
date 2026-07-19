// chatterd v2 (C++23) — UDP multicast peer discovery + a TCP chat exchange.
// Grows the same chatterd program introduced in chapter 21: the TCP frame is
// the canonical chatterd chat frame (2-byte magic "CH" + 1-byte version +
// 1-byte type + 2-byte big-endian length + UTF-8 payload) shared by every
// chatterd version (ch21-ch27). The UDP discovery beacon below is a
// completely separate wire object — a plain ASCII datagram, no relation to
// the chat frame's magic/version/type/length header.
//
//   chatterd discover --group 239.7.7.7 --port 51888 --name alice \
//       --tcp-port 9101 --iface 127.0.0.1 [--announce-ms 200] [--rounds 10]
//
// Every socket lives inside an RAII Fd, so there is no naked close(); fallible
// operations return std::expected; the listen loops run on jthreads that stop
// through their stop_token; output goes through std::println.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <mutex>
#include <optional>
#include <print>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kBeaconMagic = "CHATTERD1";  // UDP beacon tag only

// --- canonical chatterd chat frame (TCP) -----------------------------------
// magic 'C' 'H', version 0x01, type (1 byte), length (2 bytes big-endian,
// 0..65535), payload (UTF-8, type-specific). This header is byte-identical
// across every chatterd version; ch23's post-discovery greeting uses only
// JOIN (announce the dialer's name) and DELIVER (name + NUL + text, the
// listener's reply) — the same types chapters 21/22/24/27 share.
constexpr std::uint8_t kFrameMagic0 = 0x43;  // 'C'
constexpr std::uint8_t kFrameMagic1 = 0x48;  // 'H'
constexpr std::uint8_t kFrameVersion = 0x01;
constexpr std::uint8_t kFrameJoin = 1;
constexpr std::uint8_t kFrameMsg = 2;
constexpr std::uint8_t kFrameDeliver = 3;
constexpr std::uint8_t kFrameWelcome = 4;
constexpr std::uint8_t kFramePing = 5;
constexpr std::size_t kMaxFramePayload = 0xFFFF;

// --- RAII around every descriptor -----------------------------------------
class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
  Fd& operator=(Fd&& o) noexcept {
    if (this != &o) {
      reset();
      fd_ = o.fd_;
      o.fd_ = -1;
    }
    return *this;
  }
  ~Fd() { reset(); }
  [[nodiscard]] int get() const { return fd_; }
  [[nodiscard]] explicit operator bool() const { return fd_ >= 0; }
  void reset() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

 private:
  int fd_ = -1;
};

std::string errno_msg(std::string_view what) {
  return std::string(what) + ": " + std::strerror(errno);
}

// --- canonical chatterd chat frame I/O --------------------------------------
std::expected<void, std::string> write_all(int fd, const void* buf, size_t n) {
  const auto* p = static_cast<const char*>(buf);
  while (n > 0) {
    ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(errno_msg("send"));
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
  return {};
}

std::expected<void, std::string> read_all(int fd, void* buf, size_t n) {
  auto* p = static_cast<char*>(buf);
  while (n > 0) {
    ssize_t r = ::recv(fd, p, n, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return std::unexpected(errno_msg("recv"));
    }
    if (r == 0) return std::unexpected(std::string("recv: peer closed early"));
    p += r;
    n -= static_cast<size_t>(r);
  }
  return {};
}

std::expected<void, std::string> write_frame(int fd, std::uint8_t type,
                                             std::string_view payload) {
  if (payload.size() > kMaxFramePayload)
    return std::unexpected(std::string("frame payload too large: ") +
                            std::to_string(payload.size()));
  std::uint8_t hdr[6] = {
      kFrameMagic0, kFrameMagic1, kFrameVersion, type,
      static_cast<std::uint8_t>((payload.size() >> 8) & 0xFF),
      static_cast<std::uint8_t>(payload.size() & 0xFF),
  };
  if (auto r = write_all(fd, hdr, sizeof(hdr)); !r) return r;
  return write_all(fd, payload.data(), payload.size());
}

std::expected<std::pair<std::uint8_t, std::string>, std::string> read_frame(int fd) {
  std::uint8_t hdr[6];
  if (auto r = read_all(fd, hdr, sizeof(hdr)); !r) return std::unexpected(r.error());
  if (hdr[0] != kFrameMagic0 || hdr[1] != kFrameMagic1)
    return std::unexpected(std::string("bad frame magic"));
  if (hdr[2] != kFrameVersion)
    return std::unexpected(std::string("unsupported frame version: ") +
                            std::to_string(hdr[2]));
  std::uint8_t type = hdr[3];
  std::uint16_t n = static_cast<std::uint16_t>((hdr[4] << 8) | hdr[5]);
  std::string payload(n, '\0');
  if (n > 0) {
    if (auto r = read_all(fd, payload.data(), n); !r)
      return std::unexpected(r.error());
  }
  return std::make_pair(type, std::move(payload));
}

// --- socket construction ---------------------------------------------------
std::expected<in_addr, std::string> parse_ipv4(std::string_view s) {
  in_addr a{};
  std::string tmp(s);
  if (::inet_pton(AF_INET, tmp.c_str(), &a) != 1)
    return std::unexpected(std::string("bad IPv4 address: ") + tmp);
  return a;
}

void set_rcv_timeout(int fd, int ms) {
  timeval tv{.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

std::expected<Fd, std::string> make_multicast_socket(in_addr group, in_addr iface,
                                                     std::uint16_t port) {
  Fd fd(::socket(AF_INET, SOCK_DGRAM, 0));
  if (!fd) return std::unexpected(errno_msg("socket(udp)"));
  int one = 1;
  if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
    return std::unexpected(errno_msg("SO_REUSEADDR"));

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_port = htons(port);
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0)
    return std::unexpected(errno_msg("bind(udp)"));

  ip_mreq mreq{};
  mreq.imr_multiaddr = group;
  mreq.imr_interface = iface;
  if (::setsockopt(fd.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    return std::unexpected(errno_msg("IP_ADD_MEMBERSHIP"));
  if (::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface)) < 0)
    return std::unexpected(errno_msg("IP_MULTICAST_IF"));
  unsigned char loop = 1;
  if (::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0)
    return std::unexpected(errno_msg("IP_MULTICAST_LOOP"));
  unsigned char ttl = 1;
  if (::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0)
    return std::unexpected(errno_msg("IP_MULTICAST_TTL"));
  set_rcv_timeout(fd.get(), 200);
  return fd;
}

std::expected<Fd, std::string> make_tcp_listener(in_addr iface, std::uint16_t port) {
  Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!fd) return std::unexpected(errno_msg("socket(tcp)"));
  int one = 1;
  ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr = iface;
  addr.sin_port = htons(port);
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    return std::unexpected(errno_msg("bind(tcp)"));
  if (::listen(fd.get(), 16) < 0) return std::unexpected(errno_msg("listen"));
  set_rcv_timeout(fd.get(), 200);
  return fd;
}

std::expected<Fd, std::string> dial_tcp(in_addr ip, std::uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr = ip;
  addr.sin_port = htons(port);
  // Retry: the peer's beacon may arrive a hair before its listener is ready.
  for (int attempt = 0; attempt < 40; ++attempt) {
    Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd) return std::unexpected(errno_msg("socket(dial)"));
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
      return fd;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return std::unexpected(std::string("connect: giving up"));
}

// --- config ----------------------------------------------------------------
struct Config {
  std::string group;
  std::uint16_t port = 0;
  std::string name;
  std::uint16_t tcp_port = 9101;
  std::string iface = "127.0.0.1";
  int announce_ms = 200;
  int rounds = 10;
};

std::optional<std::uint16_t> parse_u16(std::string_view s) {
  int v = 0;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size() || v < 1 || v > 65535)
    return std::nullopt;
  return static_cast<std::uint16_t>(v);
}

std::optional<int> parse_int(std::string_view s) {
  int v = 0;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || p != s.data() + s.size() || v < 0) return std::nullopt;
  return v;
}

void usage() {
  std::println(stderr,
               "usage: chatterd discover --group <ip> --port <n> --name <s> "
               "[--tcp-port <n>] [--iface <ip>] [--announce-ms <n>] [--rounds <n>]");
}

std::expected<Config, int> parse_args(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) != "discover") {
    usage();
    return std::unexpected(2);
  }
  Config c;
  for (int i = 2; i < argc; ++i) {
    std::string_view a = argv[i];
    auto need = [&](std::string_view name) -> const char* {
      if (i + 1 >= argc) {
        std::println(stderr, "chatterd: {} needs a value", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "--group") {
      const char* v = need(a);
      if (!v) return std::unexpected(2);
      if (!parse_ipv4(v)) { usage(); return std::unexpected(2); }
      c.group = v;
    } else if (a == "--port") {
      const char* v = need(a);
      auto p = v ? parse_u16(v) : std::nullopt;
      if (!p) { usage(); return std::unexpected(2); }
      c.port = *p;
    } else if (a == "--name") {
      const char* v = need(a);
      if (!v) return std::unexpected(2);
      c.name = v;
    } else if (a == "--tcp-port") {
      const char* v = need(a);
      auto p = v ? parse_u16(v) : std::nullopt;
      if (!p) { usage(); return std::unexpected(2); }
      c.tcp_port = *p;
    } else if (a == "--iface") {
      const char* v = need(a);
      if (!v) return std::unexpected(2);
      if (!parse_ipv4(v)) { usage(); return std::unexpected(2); }
      c.iface = v;
    } else if (a == "--announce-ms") {
      const char* v = need(a);
      auto p = v ? parse_int(v) : std::nullopt;
      if (!p || *p == 0) { usage(); return std::unexpected(2); }
      c.announce_ms = *p;
    } else if (a == "--rounds") {
      const char* v = need(a);
      auto p = v ? parse_int(v) : std::nullopt;
      if (!p || *p == 0) { usage(); return std::unexpected(2); }
      c.rounds = *p;
    } else {
      std::println(stderr, "chatterd: unknown argument: {}", a);
      usage();
      return std::unexpected(2);
    }
  }
  if (c.group.empty() || c.port == 0 || c.name.empty()) {
    usage();
    return std::unexpected(2);
  }
  return c;
}

// Shared dedup set for discovered peer names.
struct Seen {
  std::mutex m;
  std::unordered_set<std::string> names;
  bool first(const std::string& n) {
    std::scoped_lock lk(m);
    return names.insert(n).second;
  }
};

// Split a DELIVER payload "<name>\0<text>".
std::pair<std::string, std::string> split_nul(const std::string& s) {
  auto pos = s.find('\0');
  if (pos == std::string::npos) return {s, ""};
  return {s.substr(0, pos), s.substr(pos + 1)};
}

}  // namespace

int main(int argc, char** argv) {
  auto cfg = parse_args(argc, argv);
  if (!cfg) return cfg.error();
  const Config& c = *cfg;

  auto group = parse_ipv4(c.group);
  if (!group) {
    std::println(stderr, "chatterd: error: {}", group.error());
    return 1;
  }
  auto iface = parse_ipv4(c.iface);
  if (!iface) {
    std::println(stderr, "chatterd: error: {}", iface.error());
    return 1;
  }

  auto tcp = make_tcp_listener(*iface, c.tcp_port);
  if (!tcp) {
    std::println(stderr, "chatterd: error: {}", tcp.error());
    return 1;
  }
  auto udp = make_multicast_socket(*group, *iface, c.port);
  if (!udp) {
    std::println(stderr, "chatterd: error: {}", udp.error());
    return 1;
  }

  std::println(stderr, "chatterd: announcing as {} on {}:{} (tcp {}:{})", c.name,
               c.group, c.port, c.iface, c.tcp_port);

  Seen seen;
  // DELIVER payload sent back to whoever dials us: "<name>\0hello from <name>".
  const std::string deliver_payload = c.name + '\0' + "hello from " + c.name;

  // TCP accept loop: read the dialer's JOIN frame, reply with our DELIVER
  // frame (our name + NUL + greeting text), close.
  std::jthread accepter([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      int cfd = ::accept(tcp->get(), nullptr, nullptr);
      if (cfd < 0) continue;  // timeout or interrupt -> re-check stop
      Fd conn(cfd);
      if (auto f = read_frame(conn.get()); f) {
        (void)write_frame(conn.get(), kFrameDeliver, deliver_payload);
      }
    }
  });

  // UDP receive loop: on each new peer, print + dial + exchange one message.
  std::jthread receiver([&](std::stop_token stop) {
    std::vector<char> buf(2048);
    while (!stop.stop_requested()) {
      ssize_t n = ::recv(udp->get(), buf.data(), buf.size() - 1, 0);
      if (n <= 0) continue;
      std::string msg(buf.data(), static_cast<size_t>(n));
      // Parse "CHATTERD1 <name> <tcp-port> <host-ip>".
      std::vector<std::string> tok;
      size_t start = 0;
      while (start < msg.size() && tok.size() < 4) {
        size_t sp = msg.find(' ', start);
        if (sp == std::string::npos) sp = msg.size();
        tok.emplace_back(msg.substr(start, sp - start));
        start = sp + 1;
      }
      if (tok.size() != 4 || tok[0] != kBeaconMagic) continue;
      const std::string& pname = tok[1];
      if (pname == c.name) continue;  // our own beacon
      auto pport = parse_u16(tok[2]);
      auto pip = parse_ipv4(tok[3]);
      if (!pport || !pip) continue;
      if (!seen.first(pname)) continue;  // already discovered

      std::println("discovered peer {} at {}:{}", pname, tok[3], *pport);
      auto conn = dial_tcp(*pip, *pport);
      if (!conn) {
        std::println(stderr, "chatterd: error: dial {}: {}", pname, conn.error());
        continue;
      }
      // JOIN (announce ourselves), then read the peer's DELIVER reply.
      if (auto s = write_frame(conn->get(), kFrameJoin, c.name); !s) {
        std::println(stderr, "chatterd: error: {}", s.error());
        continue;
      }
      auto reply = read_frame(conn->get());
      if (!reply) {
        std::println(stderr, "chatterd: error: {}", reply.error());
        continue;
      }
      auto [rname, rtext] = split_nul(reply->second);
      std::println("peer {} says: {}", rname, rtext);
    }
  });

  // Announce loop: periodic multicast beacons, then a grace window for the
  // final exchanges to complete before we stop the listen threads.
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_addr = *group;
  dst.sin_port = htons(c.port);
  std::string beacon = std::string(kBeaconMagic) + " " + c.name + " " +
                       std::to_string(c.tcp_port) + " " + c.iface;
  for (int r = 0; r < c.rounds; ++r) {
    ::sendto(udp->get(), beacon.data(), beacon.size(), 0,
             reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    std::this_thread::sleep_for(std::chrono::milliseconds(c.announce_ms));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(800));

  receiver.request_stop();
  accepter.request_stop();
  return 0;
}
