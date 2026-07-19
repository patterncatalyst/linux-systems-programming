// pmon v3 — identity & privilege (chapter 14).
//
// Subcommands:
//   pmon drop --user <name> [--keep-cap net_bind_service] -- CMD [args...]
//   pmon bindprobe [--port 80]
//
// `drop` runs as root and hands CMD to an UNPRIVILEGED user, optionally
// carrying exactly one capability (CAP_NET_BIND_SERVICE) across the identity
// change and the following execve(2) via the ambient-capability mechanism.
// The whole lesson is the ORDER of operations:
//
//   1. prctl(PR_SET_KEEPCAPS, 1)   — before the uid change, so the permitted
//                                    set survives setresuid(2) instead of
//                                    being scrubbed when uid goes 0 -> !0.
//   2. setgroups / setresgid / setresuid   — become the target user.
//   3. capset(2)                   — pin CAP_NET_BIND_SERVICE into the
//                                    permitted AND inheritable sets (ambient
//                                    requires both), dropping everything else.
//   4. prctl(PR_CAP_AMBIENT_RAISE) — raise it into the ambient set, the only
//                                    set that a non-root, non-file-cap execve
//                                    preserves.
//   5. execvp(2)                   — CMD now runs as <name> yet keeps :80.
//
// Raw prctl(2) / capset(2) / setres*id(2) on purpose — no libcap. RAII owns
// the socket; std::expected carries syscall failures; nothing is handled the
// C way (no scattered perror/exit). Deliberately single-threaded: a privilege
// drop must touch every task, and execve() only makes sense from one thread.

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <grp.h>
#include <linux/capability.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

// A syscall failure: what we were attempting, plus the captured errno.
struct SysErr {
  std::string what;
  int err;
};

// Wrap a raw syscall return: <0 becomes an error carrying errno.
std::expected<void, SysErr> checked(long rc, std::string_view what) {
  if (rc < 0) {
    return std::unexpected(SysErr{std::string(what), errno});
  }
  return {};
}

// RAII owner for a file descriptor — closed exactly once on scope exit.
class Fd {
 public:
  explicit Fd(int fd) noexcept : fd_(fd) {}
  ~Fd() {
    if (fd_ >= 0) ::close(fd_);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Fd& operator=(Fd&&) = delete;

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

 private:
  int fd_ = -1;
};

// ---- bindprobe -----------------------------------------------------------
//
// Bind a TCP socket on <port> and report the effective uid. The whole point:
// binding a port < 1024 is impossible for a non-root process UNLESS it holds
// CAP_NET_BIND_SERVICE. Success at uid!=0 on :80 is the proof the ambient cap
// survived. Failure prints a stable "Permission denied" line and exits 3.
int cmd_bindprobe(std::span<char*> args) {
  int port = 80;
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    if (a == "--port" && i + 1 < args.size()) {
      port = std::atoi(args[++i]);
    } else {
      std::println(stderr, "bindprobe: unexpected argument: {}", a);
      return 2;
    }
  }

  Fd sock(::socket(AF_INET, SOCK_STREAM, 0));
  if (!sock.valid()) {
    std::println(stderr, "bindprobe: socket: {}", std::strerror(errno));
    return 1;
  }
  int one = 1;
  ::setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));

  if (::bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::println(stderr, "bindprobe: bind :{}: {}", port, std::strerror(errno));
    return 3;
  }
  std::println("bindprobe: uid={} bound :{}", ::getuid(), port);
  return 0;
}

// ---- drop ----------------------------------------------------------------

// Perform the identity change (and optional cap retention), then execvp CMD.
// Returns only on failure; on success control never comes back.
std::expected<void, SysErr> arrange_and_exec(const passwd& pw, bool keep_cap,
                                             std::span<char*> cmd) {
  // (1) Retain the permitted set across the coming uid change. Must precede
  //     setresuid — after it, it is too late.
  if (keep_cap) {
    if (auto r = checked(::prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0), "PR_SET_KEEPCAPS");
        !r)
      return r;
  }

  // (2) Become the target user: groups first, gid before uid — once uid drops
  //     we can no longer change gids.
  gid_t gid = pw.pw_gid;
  if (auto r = checked(::setgroups(1, &gid), "setgroups"); !r) return r;
  if (auto r = checked(::setresgid(gid, gid, gid), "setresgid"); !r) return r;
  if (auto r = checked(::setresuid(pw.pw_uid, pw.pw_uid, pw.pw_uid), "setresuid");
      !r)
    return r;

  // (3) Pin exactly CAP_NET_BIND_SERVICE into permitted+inheritable (ambient
  //     demands the cap live in both), dropping every other retained cap.
  if (keep_cap) {
    __user_cap_header_struct hdr{};
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    hdr.pid = 0;
    std::array<__user_cap_data_struct, 2> data{};
    const std::uint32_t bit = 1u << CAP_NET_BIND_SERVICE;  // cap 10, word 0
    data[0].effective = bit;
    data[0].permitted = bit;
    data[0].inheritable = bit;
    if (auto r = checked(::syscall(SYS_capset, &hdr, data.data()), "capset"); !r)
      return r;

    // (4) Raise it into the ambient set — the only set an unprivileged execve
    //     of a file without file-caps carries forward.
    if (auto r = checked(::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE,
                                 CAP_NET_BIND_SERVICE, 0, 0),
                         "PR_CAP_AMBIENT_RAISE");
        !r)
      return r;
  }

  // (5) Hand off. CMD now runs as <name>, keeping :80 only if we raised it.
  std::vector<char*> args(cmd.begin(), cmd.end());
  args.push_back(nullptr);
  ::execvp(args[0], args.data());
  return std::unexpected(SysErr{std::string("execvp ") + args[0], errno});
}

int cmd_drop(std::span<char*> args) {
  std::string_view user;
  bool keep_cap = false;
  std::span<char*> cmd;
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    if (a == "--user" && i + 1 < args.size()) {
      user = args[++i];
    } else if (a == "--keep-cap" && i + 1 < args.size()) {
      std::string_view cap = args[++i];
      if (cap != "net_bind_service") {
        std::println(stderr, "drop: unsupported --keep-cap: {}", cap);
        return 2;
      }
      keep_cap = true;
    } else if (a == "--") {
      cmd = args.subspan(i + 1);
      break;
    } else {
      std::println(stderr, "drop: unexpected argument: {}", a);
      return 2;
    }
  }
  if (user.empty()) {
    std::println(stderr, "drop: --user <name> is required");
    return 2;
  }
  if (cmd.empty()) {
    std::println(stderr, "drop: missing -- CMD");
    return 2;
  }
  if (::getuid() != 0) {
    std::println(stderr, "drop: must run as root");
    return 1;
  }

  errno = 0;
  passwd* pw = ::getpwnam(std::string(user).c_str());
  if (pw == nullptr) {
    std::println(stderr, "drop: unknown user: {}", user);
    return 1;
  }

  auto r = arrange_and_exec(*pw, keep_cap, cmd);
  // Only reached if a step failed (exec never returns on success).
  std::println(stderr, "drop: {}: {}", r.error().what, std::strerror(r.error().err));
  return 1;
}

void usage() {
  std::println(stderr,
               "usage:\n"
               "  pmon drop --user <name> [--keep-cap net_bind_service] -- CMD [args...]\n"
               "  pmon bindprobe [--port 80]");
}

}  // namespace

int main(int argc, char** argv) {
  std::span<char*> all(argv, static_cast<std::size_t>(argc));
  if (all.size() < 2) {
    usage();
    return 2;
  }
  std::string_view sub = all[1];
  std::span<char*> rest = all.subspan(2);
  if (sub == "drop") return cmd_drop(rest);
  if (sub == "bindprobe") return cmd_bindprobe(rest);
  usage();
  return 2;
}
