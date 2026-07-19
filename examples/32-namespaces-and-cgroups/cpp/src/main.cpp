// pmon v6 — namespaces & cgroups (chapter 32).
//
// Subcommand:
//   pmon containerize [--hostname NAME] [--mem-max BYTES|max]
//                      [--cpu-max "QUOTA PERIOD"|max] [--cgroup NAME]
//                      -- CMD [ARGS...]
//
// `containerize` builds the two primitives every real container runtime is
// made of, by hand:
//
//   1. NAMESPACES — unshare(2) with CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWNET|
//      CLONE_NEWPID puts the calling process into a fresh mount, UTS and
//      network namespace immediately, and arranges for the NEXT clone3(2)
//      child to become PID 1 of a fresh PID namespace (unshare(2) never
//      moves the caller itself into a new PID namespace — only its future
//      children).
//   2. CGROUP v2 LIMITS — a dedicated cgroup gets memory.max and cpu.max
//      written before the child is ever created, so the limits are already
//      live the instant CMD execs.
//
// Raw clone3(2) via <sys/syscall.h> on purpose (no libc clone() wrapper,
// no fork()/vfork() distinction to reason about, and it is the syscall the
// chapter is actually teaching). RAII closes every fd; std::expected carries
// every syscall failure; single-threaded throughout (mixing threads with
// unshare()/clone3() is its own footgun — see the Go build's comment for
// what goes wrong when a runtime does that under you).

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

#include <csignal>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
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

// clone3(2)'s argument struct. We deliberately declare only the
// CLONE_ARGS_SIZE_VER0 prefix (through `tls`) — <linux/sched.h> redefines
// the CLONE_NEW* macros already pulled in by <sched.h>, so we avoid including
// it and hand-roll the layout the syscall actually reads.
struct clone_args_v0 {
  std::uint64_t flags;
  std::uint64_t pidfd;
  std::uint64_t child_tid;
  std::uint64_t parent_tid;
  std::uint64_t exit_signal;
  std::uint64_t stack;
  std::uint64_t stack_size;
  std::uint64_t tls;
};

// ---- tiny sysfs/cgroupfs helpers -----------------------------------------

std::expected<void, SysErr> write_file(const std::string& path, std::string_view data) {
  Fd fd(::open(path.c_str(), O_WRONLY));
  if (!fd.valid()) return std::unexpected(SysErr{"open " + path, errno});
  if (auto r = checked(::write(fd.get(), data.data(), data.size()), "write " + path); !r) {
    return r;
  }
  return {};
}

std::expected<std::string, SysErr> read_file(const std::string& path) {
  Fd fd(::open(path.c_str(), O_RDONLY));
  if (!fd.valid()) return std::unexpected(SysErr{"open " + path, errno});
  std::array<char, 4096> buf{};
  ssize_t n = ::read(fd.get(), buf.data(), buf.size());
  if (n < 0) return std::unexpected(SysErr{"read " + path, errno});
  return std::string(buf.data(), static_cast<std::size_t>(n));
}

// Whether `tokens` (a whitespace-separated list, as cgroupfs writes it)
// contains `word` as a whole token — "cpu" must not match inside "cpuset".
bool has_token(std::string_view tokens, std::string_view word) {
  std::size_t pos = 0;
  while (pos < tokens.size()) {
    std::size_t end = tokens.find_first_of(" \n\t", pos);
    if (end == std::string_view::npos) end = tokens.size();
    if (tokens.substr(pos, end - pos) == word) return true;
    pos = end + 1;
  }
  return false;
}

// The root cgroup is exempt from the "no internal process" constraint, so we
// can enable controllers there even though it holds every process on the
// box — this is exactly how a fresh sibling cgroup gets memory/cpu control
// without first relocating ourselves into a leaf.
std::expected<void, SysErr> ensure_root_controllers() {
  auto have = read_file("/sys/fs/cgroup/cgroup.subtree_control");
  if (!have) return std::unexpected(have.error());
  bool needs_mem = !has_token(*have, "memory");
  bool needs_cpu = !has_token(*have, "cpu");
  if (!needs_mem && !needs_cpu) return {};
  std::string add;
  if (needs_mem) add += "+memory ";
  if (needs_cpu) add += "+cpu ";
  return write_file("/sys/fs/cgroup/cgroup.subtree_control", add);
}

// Create (or reuse) /sys/fs/cgroup/<name>, apply the limits, and move the
// calling process into it. Every subsequent clone3(2) child inherits cgroup
// membership automatically, so CMD is under the limits from its first
// instruction.
std::expected<std::string, SysErr> setup_cgroup(const std::string& name,
                                                const std::string& mem_max,
                                                const std::string& cpu_max) {
  if (auto r = ensure_root_controllers(); !r) return std::unexpected(r.error());

  std::string path = "/sys/fs/cgroup/" + name;
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    return std::unexpected(SysErr{"mkdir " + path, errno});
  }
  if (auto r = write_file(path + "/memory.max", mem_max); !r) return std::unexpected(r.error());
  if (auto r = write_file(path + "/cpu.max", cpu_max); !r) return std::unexpected(r.error());
  // Best-effort: no swap headroom, so a breach of memory.max is a real OOM
  // kill instead of a silent slowdown via swap. Not every kernel exposes
  // swap accounting the same way, so a failure here isn't fatal.
  (void)write_file(path + "/memory.swap.max", "0");

  std::string pid_line = std::to_string(::getpid());
  if (auto r = write_file(path + "/cgroup.procs", pid_line); !r) return std::unexpected(r.error());

  return path;
}

// Names as printed in "killed signal=<n> (<NAME>)". A hand-rolled table
// (identical across all three implementations and to pmon's earlier
// chapters) rather than strsignal(3), whose text is locale-dependent prose.
[[nodiscard]] std::string signal_name(int sig) {
  static constexpr std::array<std::string_view, 32> names{
      "",       "HUP",  "INT",  "QUIT", "ILL",  "TRAP", "ABRT",   "BUS",
      "FPE",    "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM",   "TERM",
      "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU",   "URG",
      "XCPU",   "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR",   "SYS"};
  if (sig >= 1 && sig < static_cast<int>(names.size())) {
    return std::string{names[sig]};
  }
  return "SIG" + std::to_string(sig);
}

// Parse "some avg10=X avg60=Y avg300=Z total=T\n..." and return X.
std::string parse_psi_some_avg10(const std::string& psi) {
  auto some_pos = psi.find("some ");
  if (some_pos == std::string::npos) return "?";
  auto avg_pos = psi.find("avg10=", some_pos);
  if (avg_pos == std::string::npos) return "?";
  avg_pos += std::strlen("avg10=");
  auto end = psi.find(' ', avg_pos);
  return psi.substr(avg_pos, end == std::string::npos ? std::string::npos : end - avg_pos);
}

// ---- containerize ----------------------------------------------------------

int cmd_containerize(std::span<char*> args) {
  std::string hostname = "pmon-containerized";
  std::string mem_max = "max";
  std::string cpu_max = "max 100000";
  std::string cgroup_name;
  std::span<char*> cmd;

  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    if (a == "--hostname" && i + 1 < args.size()) {
      hostname = args[++i];
    } else if (a == "--mem-max" && i + 1 < args.size()) {
      mem_max = args[++i];
    } else if (a == "--cpu-max" && i + 1 < args.size()) {
      cpu_max = args[++i];
    } else if (a == "--cgroup" && i + 1 < args.size()) {
      cgroup_name = args[++i];
    } else if (a == "--") {
      cmd = args.subspan(i + 1);
      break;
    } else {
      std::println(stderr, "containerize: unexpected argument: {}", a);
      return 2;
    }
  }
  if (cmd.empty()) {
    std::println(stderr, "containerize: missing -- CMD");
    return 2;
  }
  if (::getuid() != 0) {
    std::println(stderr, "containerize: must run as root");
    return 1;
  }
  if (cgroup_name.empty()) {
    cgroup_name = "pmon-" + std::to_string(::getpid());
  }

  auto cg = setup_cgroup(cgroup_name, mem_max, cpu_max);
  if (!cg) {
    std::println(stderr, "containerize: {}: {}", cg.error().what, std::strerror(cg.error().err));
    return 1;
  }
  const std::string cgroup_path = *cg;

  // Mount, UTS and network namespaces take effect on THIS process right now.
  // The PID namespace is deferred: the next clone3(2) child becomes PID 1 of
  // a fresh one; we (the caller) stay in our original PID namespace so we
  // can waitpid(2) the child the ordinary way.
  if (auto r = checked(::unshare(CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID),
                       "unshare");
      !r) {
    std::println(stderr, "containerize: {}: {}", r.error().what, std::strerror(r.error().err));
    return 1;
  }

  // Detach mount propagation recursively so nothing we do in this namespace
  // (or CMD does) leaks a mount event back to the host's mount table.
  if (auto r = checked(::mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr),
                       "mount MS_PRIVATE");
      !r) {
    std::println(stderr, "containerize: {}: {}", r.error().what, std::strerror(r.error().err));
    return 1;
  }

  // sethostname(2) now lands in the fresh UTS namespace, not the host's.
  if (auto r = checked(::sethostname(hostname.c_str(), hostname.size()), "sethostname"); !r) {
    std::println(stderr, "containerize: {}: {}", r.error().what, std::strerror(r.error().err));
    return 1;
  }

  // clone3(2): flags=0 (the namespace work is already done above), stack=0
  // (kernel copy-on-write duplicates our stack exactly like fork(2)),
  // exit_signal=SIGCHLD (so waitpid(2) below works the ordinary way).
  clone_args_v0 cl{};
  cl.exit_signal = SIGCHLD;
  long rc = ::syscall(SYS_clone3, &cl, sizeof(cl));
  if (rc < 0) {
    std::println(stderr, "containerize: clone3: {}", std::strerror(errno));
    return 1;
  }

  if (rc == 0) {
    // --- child: PID 1 of the new PID namespace ---
    if (::getpid() == 1) {
      std::println("pmon: child sees pid 1");
    }
    std::fflush(stdout);

    char buf[256]{};
    if (::gethostname(buf, sizeof(buf) - 1) == 0) {
      std::println("pmon: hostname={}", buf);
    }
    std::fflush(stdout);

    std::vector<char*> argv(cmd.begin(), cmd.end());
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    std::println(stderr, "pmon: exec {}: {}", argv[0], std::strerror(errno));
    std::fflush(stderr);
    _exit(127);
  }

  // --- parent: waits, then reports the cgroup-side evidence ---
  int status = 0;
  if (::waitpid(static_cast<pid_t>(rc), &status, 0) < 0) {
    std::println(stderr, "containerize: waitpid: {}", std::strerror(errno));
    return 1;
  }

  auto psi = read_file(cgroup_path + "/memory.pressure");
  if (psi) {
    std::println("pmon: cgroup mem.pressure some={}", parse_psi_some_avg10(*psi));
  }

  int exit_code = 0;
  if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);
    std::println("pmon: child exited status={}", code);
    exit_code = code;
  } else if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    std::println("pmon: child killed signal={} ({})", sig, signal_name(sig));
    exit_code = 128 + sig;
  }

  // Best-effort cleanup; a live cgroup left behind is harmless (reused by
  // its own name next run) and not worth failing the exit code over.
  ::rmdir(cgroup_path.c_str());

  return exit_code;
}

void usage() {
  std::println(stderr,
               "usage:\n"
               "  pmon containerize [--hostname NAME] [--mem-max BYTES|max]\n"
               "                    [--cpu-max \"QUOTA PERIOD\"|max] [--cgroup NAME]\n"
               "                    -- CMD [ARGS...]");
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
  if (sub == "containerize") return cmd_containerize(rest);
  usage();
  return 2;
}
