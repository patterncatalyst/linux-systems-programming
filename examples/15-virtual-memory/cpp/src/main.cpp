// memmap: self-inspecting virtual-memory tool — perform one kind of
// allocation, touch its pages, then report what the kernel now shows for
// this very process: the /proc/self/maps region backing the allocation,
// the VmRSS growth, and the getrusage(2) page-fault deltas.
//
//   memmap --mode stack|heap|mmap-anon|mmap-file <FILE>|fault-walk [--mb N]
//
//   stack      touch a local buffer on the call stack (clamped to 4 MiB)
//   heap       allocator memory (operator new via std::vector)
//   mmap-anon  private anonymous mmap(2), written one byte per page
//   mmap-file  read-only private mmap of FILE, read one byte per page
//   fault-walk create a temp file of N MiB, map it, touch it in 8 steps,
//              printing the minor/major fault growth after every step
//
// Output contract (identical across C++, Go, Rust):
//   memmap: mode=<m> bytes=<b> pages=<p>
//   [fault-walk only] memmap: walk file=<path> steps=8
//   [fault-walk only] memmap: step=<i>/8 pages=<p> minor=<d> major=<d>
//   memmap: maps excerpt
//   memmap:   <raw /proc/self/maps line>   <-- target (mode=<m>)
//   memmap: vmrss_before=<kb>KB vmrss_after=<kb>KB
//   memmap: faults minor=<n> major=<n>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr std::size_t kMiB = 1024 * 1024;
constexpr std::size_t kStackCapMb = 4;  // stays well inside the 8 MiB rlimit
constexpr int kWalkSteps = 8;

enum class Mode { stack, heap, mmap_anon, mmap_file, fault_walk };

[[nodiscard]] constexpr std::string_view mode_name(Mode m) {
    switch (m) {
    case Mode::stack:      return "stack";
    case Mode::heap:       return "heap";
    case Mode::mmap_anon:  return "mmap-anon";
    case Mode::mmap_file:  return "mmap-file";
    case Mode::fault_walk: return "fault-walk";
    }
    return "?";
}

[[nodiscard]] std::string errno_message(int err) {
    return std::error_code(err, std::system_category()).message();
}

// Keep the optimizer from proving the touched buffer unread.
inline void opt_barrier(const void* p) {
    __asm__ volatile("" : : "r"(p) : "memory");
}

// ---------------------------------------------------------------------------
// RAII wrappers
// ---------------------------------------------------------------------------

class Fd {
public:
    explicit Fd(int fd = -1) noexcept : fd_{fd} {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_{std::exchange(o.fd_, -1)} {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

private:
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    int fd_ = -1;
};

class Mapping {
public:
    Mapping(void* p, std::size_t len) noexcept : p_{p}, len_{len} {}
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Mapping(Mapping&& o) noexcept
        : p_{std::exchange(o.p_, nullptr)}, len_{std::exchange(o.len_, 0)} {}
    ~Mapping() {
        if (p_ != nullptr) {
            ::munmap(p_, len_);
        }
    }
    [[nodiscard]] std::byte* data() const noexcept {
        return static_cast<std::byte*>(p_);
    }
    [[nodiscard]] std::size_t size() const noexcept { return len_; }

private:
    void* p_ = nullptr;
    std::size_t len_ = 0;
};

// Unlinks its path on scope exit.
class TempFile {
public:
    explicit TempFile(std::string path) : path_{std::move(path)} {}
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    ~TempFile() {
        if (!path_.empty()) {
            ::unlink(path_.c_str());
        }
    }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
};

// ---------------------------------------------------------------------------
// /proc and getrusage probes
// ---------------------------------------------------------------------------

[[nodiscard]] std::expected<long, std::string> vmrss_kb() {
    std::ifstream f{"/proc/self/status"};
    if (!f) {
        return std::unexpected{"cannot open /proc/self/status"};
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.starts_with("VmRSS:")) {
            std::string_view rest{line};
            rest.remove_prefix(6);
            std::size_t i = 0;
            while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) ++i;
            long kb = 0;
            auto [ptr, ec] =
                std::from_chars(rest.data() + i, rest.data() + rest.size(), kb);
            if (ec != std::errc{}) {
                return std::unexpected{"cannot parse VmRSS"};
            }
            return kb;
        }
    }
    return std::unexpected{"VmRSS not found in /proc/self/status"};
}

struct Faults {
    long minor = 0;
    long major = 0;
};

[[nodiscard]] std::expected<Faults, std::string> fault_counts() {
    rusage ru{};
    if (::getrusage(RUSAGE_SELF, &ru) != 0) {
        return std::unexpected{"getrusage: " + errno_message(errno)};
    }
    return Faults{ru.ru_minflt, ru.ru_majflt};
}

// Print every /proc/self/maps line whose range overlaps [addr, addr+len).
[[nodiscard]] std::expected<void, std::string>
print_maps_excerpt(std::uintptr_t addr, std::size_t len, Mode mode) {
    std::ifstream f{"/proc/self/maps"};
    if (!f) {
        return std::unexpected{"cannot open /proc/self/maps"};
    }
    std::println("memmap: maps excerpt");
    const std::uintptr_t lo = addr;
    const std::uintptr_t hi = addr + len;
    std::string line;
    while (std::getline(f, line)) {
        const auto dash = line.find('-');
        const auto sp = line.find(' ');
        if (dash == std::string::npos || sp == std::string::npos || sp < dash) {
            continue;
        }
        std::uintptr_t start = 0;
        std::uintptr_t end = 0;
        auto r1 = std::from_chars(line.data(), line.data() + dash, start, 16);
        auto r2 =
            std::from_chars(line.data() + dash + 1, line.data() + sp, end, 16);
        if (r1.ec != std::errc{} || r2.ec != std::errc{}) {
            continue;
        }
        if (start < hi && end > lo) {
            std::println("memmap:   {}   <-- target (mode={})", line,
                         mode_name(mode));
        }
    }
    return {};
}

struct Baseline {
    long rss_kb = 0;
    Faults faults;
};

[[nodiscard]] std::expected<Baseline, std::string> take_baseline() {
    auto rss = vmrss_kb();
    if (!rss) return std::unexpected{rss.error()};
    auto fl = fault_counts();
    if (!fl) return std::unexpected{fl.error()};
    return Baseline{*rss, *fl};
}

// Common tail: maps excerpt, RSS growth, fault deltas.
[[nodiscard]] std::expected<void, std::string>
report(std::uintptr_t addr, std::size_t len, Mode mode, const Baseline& base) {
    if (auto r = print_maps_excerpt(addr, len, mode); !r) return r;
    auto rss_after = vmrss_kb();
    if (!rss_after) return std::unexpected{rss_after.error()};
    auto fl = fault_counts();
    if (!fl) return std::unexpected{fl.error()};
    std::println("memmap: vmrss_before={}KB vmrss_after={}KB", base.rss_kb,
                 *rss_after);
    std::println("memmap: faults minor={} major={}",
                 fl->minor - base.faults.minor, fl->major - base.faults.major);
    return {};
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

void touch_writable(std::span<std::byte> mem, std::size_t page) {
    for (std::size_t i = 0; i < mem.size(); i += page) {
        mem[i] = std::byte{1};
    }
    opt_barrier(mem.data());
}

// Read one byte per page; the sum flows into the barrier so it stays live.
void touch_readable(std::span<const std::byte> mem, std::size_t page) {
    unsigned long sum = 0;
    for (std::size_t i = 0; i < mem.size(); i += page) {
        sum += std::to_underlying(mem[i]);
    }
    opt_barrier(&sum);
}

// Baseline is captured by the caller so that any page the compiler touches
// while setting up this 4 MiB frame still lands inside the measured window.
[[gnu::noinline]] std::expected<void, std::string>
stack_worker(std::size_t bytes, std::size_t page, const Baseline& base) {
    std::array<std::byte, kStackCapMb * kMiB> buf{};
    touch_writable(std::span{buf.data(), bytes}, page);
    return report(reinterpret_cast<std::uintptr_t>(buf.data()), bytes,
                  Mode::stack, base);
}

[[nodiscard]] std::expected<void, std::string>
run_stack(std::size_t bytes, std::size_t page) {
    auto base = take_baseline();
    if (!base) return std::unexpected{base.error()};
    return stack_worker(bytes, page, *base);
}

[[nodiscard]] std::expected<void, std::string>
run_heap(std::size_t bytes, std::size_t page) {
    auto base = take_baseline();
    if (!base) return std::unexpected{base.error()};
    std::vector<std::byte> buf(bytes);
    touch_writable(std::span{buf}, page);
    return report(reinterpret_cast<std::uintptr_t>(buf.data()), bytes,
                  Mode::heap, *base);
}

[[nodiscard]] std::expected<void, std::string>
run_mmap_anon(std::size_t bytes, std::size_t page) {
    auto base = take_baseline();
    if (!base) return std::unexpected{base.error()};
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return std::unexpected{"mmap: " + errno_message(errno)};
    }
    Mapping map{p, bytes};
    touch_writable(std::span{map.data(), map.size()}, page);
    return report(reinterpret_cast<std::uintptr_t>(map.data()), map.size(),
                  Mode::mmap_anon, *base);
}

[[nodiscard]] std::expected<std::pair<Fd, std::size_t>, std::string>
open_for_map(const std::string& path) {
    Fd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (!fd.valid()) {
        return std::unexpected{path + ": " + errno_message(errno)};
    }
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) {
        return std::unexpected{path + ": fstat: " + errno_message(errno)};
    }
    if (st.st_size <= 0) {
        return std::unexpected{path + ": file is empty"};
    }
    return std::pair{std::move(fd), static_cast<std::size_t>(st.st_size)};
}

[[nodiscard]] std::expected<Mapping, std::string> map_readonly(int fd,
                                                               std::size_t len) {
    void* p = ::mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        return std::unexpected{"mmap: " + errno_message(errno)};
    }
    return Mapping{p, len};
}

[[nodiscard]] std::expected<void, std::string>
run_mmap_file(const std::string& path, std::size_t page) {
    auto opened = open_for_map(path);
    if (!opened) return std::unexpected{opened.error()};
    auto& [fd, len] = *opened;
    const std::size_t pages = (len + page - 1) / page;
    std::println("memmap: mode=mmap-file bytes={} pages={}", len, pages);
    auto base = take_baseline();
    if (!base) return std::unexpected{base.error()};
    auto map = map_readonly(fd.get(), len);
    if (!map) return std::unexpected{map.error()};
    touch_readable(std::span{map->data(), map->size()}, page);
    return report(reinterpret_cast<std::uintptr_t>(map->data()), map->size(),
                  Mode::mmap_file, *base);
}

[[nodiscard]] std::expected<std::string, std::string>
write_walk_file(std::size_t bytes) {
    const char* tmpdir = std::getenv("TMPDIR");
    std::string path = std::string{tmpdir != nullptr ? tmpdir : "/tmp"} +
                       "/memmap-walk-" + std::to_string(::getpid()) + ".bin";
    Fd fd{::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)};
    if (!fd.valid()) {
        return std::unexpected{path + ": " + errno_message(errno)};
    }
    std::vector<std::byte> chunk(kMiB, std::byte{0xA5});
    std::size_t left = bytes;
    while (left > 0) {
        const std::size_t n = std::min(left, chunk.size());
        std::size_t off = 0;
        while (off < n) {
            const ssize_t w = ::write(fd.get(), chunk.data() + off, n - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                return std::unexpected{path + ": write: " +
                                       errno_message(errno)};
            }
            off += static_cast<std::size_t>(w);
        }
        left -= n;
    }
    return path;
}

[[nodiscard]] std::expected<void, std::string>
run_fault_walk(std::size_t bytes, std::size_t page) {
    auto path = write_walk_file(bytes);
    if (!path) return std::unexpected{path.error()};
    TempFile tmp{*path};
    auto opened = open_for_map(tmp.path());
    if (!opened) return std::unexpected{opened.error()};
    auto& [fd, len] = *opened;
    auto base = take_baseline();
    if (!base) return std::unexpected{base.error()};
    auto map = map_readonly(fd.get(), len);
    if (!map) return std::unexpected{map.error()};

    std::println("memmap: walk file={} steps={}", tmp.path(), kWalkSteps);
    const std::size_t pages = len / page;
    Faults prev = base->faults;
    std::size_t done = 0;
    for (int step = 1; step <= kWalkSteps; ++step) {
        std::size_t quota = pages / kWalkSteps;
        if (step == kWalkSteps) quota = pages - done;
        touch_readable(std::span{map->data() + done * page, quota * page},
                       page);
        auto now = fault_counts();
        if (!now) return std::unexpected{now.error()};
        std::println("memmap: step={}/{} pages={} minor={} major={}", step,
                     kWalkSteps, quota, now->minor - prev.minor,
                     now->major - prev.major);
        prev = *now;
        done += quota;
    }
    return report(reinterpret_cast<std::uintptr_t>(map->data()), map->size(),
                  Mode::fault_walk, *base);
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

struct Options {
    Mode mode = Mode::heap;
    std::size_t mb = 64;
    std::string file;
};

void usage(std::FILE* to) {
    std::println(to,
                 "usage: memmap --mode stack|heap|mmap-anon|mmap-file <FILE>|"
                 "fault-walk [--mb N]");
}

[[nodiscard]] std::expected<Options, std::string>
parse_args(std::span<char*> args) {
    Options opts;
    bool mode_set = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a{args[i]};
        if (a == "--mode") {
            if (++i >= args.size()) {
                return std::unexpected{"--mode needs a value"};
            }
            const std::string_view m{args[i]};
            if (m == "stack")           opts.mode = Mode::stack;
            else if (m == "heap")       opts.mode = Mode::heap;
            else if (m == "mmap-anon")  opts.mode = Mode::mmap_anon;
            else if (m == "mmap-file")  opts.mode = Mode::mmap_file;
            else if (m == "fault-walk") opts.mode = Mode::fault_walk;
            else return std::unexpected{"unknown mode: " + std::string{m}};
            mode_set = true;
        } else if (a == "--mb") {
            if (++i >= args.size()) {
                return std::unexpected{"--mb needs a value"};
            }
            const std::string_view v{args[i]};
            std::size_t mb = 0;
            auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), mb);
            if (ec != std::errc{} || p != v.data() + v.size() || mb < 1 ||
                mb > 1024) {
                return std::unexpected{"--mb must be 1..1024"};
            }
            opts.mb = mb;
        } else if (!a.starts_with("--") && opts.file.empty()) {
            opts.file = a;
        } else {
            return std::unexpected{"unexpected argument: " + std::string{a}};
        }
    }
    if (!mode_set) return std::unexpected{"--mode is required"};
    if (opts.mode == Mode::mmap_file && opts.file.empty()) {
        return std::unexpected{"mmap-file needs a FILE argument"};
    }
    if (opts.mode != Mode::mmap_file && !opts.file.empty()) {
        return std::unexpected{"only mmap-file takes a FILE argument"};
    }
    return opts;
}

}  // namespace

int main(int argc, char** argv) {
    const auto opts =
        parse_args(std::span{argv + 1, static_cast<std::size_t>(argc - 1)});
    if (!opts) {
        usage(stderr);
        return 2;
    }
    const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));

    std::expected<void, std::string> r;
    switch (opts->mode) {
    case Mode::stack: {
        const std::size_t bytes = std::min(opts->mb, kStackCapMb) * kMiB;
        std::println("memmap: mode=stack bytes={} pages={}", bytes,
                     bytes / page);
        r = run_stack(bytes, page);
        break;
    }
    case Mode::heap: {
        const std::size_t bytes = opts->mb * kMiB;
        std::println("memmap: mode=heap bytes={} pages={}", bytes,
                     bytes / page);
        r = run_heap(bytes, page);
        break;
    }
    case Mode::mmap_anon: {
        const std::size_t bytes = opts->mb * kMiB;
        std::println("memmap: mode=mmap-anon bytes={} pages={}", bytes,
                     bytes / page);
        r = run_mmap_anon(bytes, page);
        break;
    }
    case Mode::mmap_file:
        r = run_mmap_file(opts->file, page);  // prints its own header line
        break;
    case Mode::fault_walk: {
        const std::size_t bytes = opts->mb * kMiB;
        std::println("memmap: mode=fault-walk bytes={} pages={}", bytes,
                     bytes / page);
        r = run_fault_walk(bytes, page);
        break;
    }
    }
    if (!r) {
        std::println(stderr, "memmap: error: {}", r.error());
        return 1;
    }
    return 0;
}
