// overheadbench: three host-local microbenchmarks used to talk about
// virtualization/container overhead in chapter 35 —
//   (a) syscall latency: a tight getpid(2) loop, ns/call
//   (b) memory bandwidth: sequential read of a big buffer, GB/s
//   (c) small-file IO: create/write/fsync/unlink loop, ops/s
//
//   overheadbench [--bench syscall|mem|io|all] [--iters N]
//
// Prints one line per bench: "bench=<name> metric=<value> unit=<unit>".
// This binary always measures wherever it runs (host, VM, or container) —
// the chapter runs the *same* binary in all three places and tabulates the
// numbers; this example itself only asserts the host numbers are sane.

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

enum class Bench { Syscall, Mem, Io, All };

struct Config {
    Bench bench = Bench::All;
    std::optional<std::uint64_t> iters;
};

constexpr std::string_view kUsage =
    "usage: overheadbench [--bench syscall|mem|io|all] [--iters N]";

// Defaults are chosen so each bench runs for roughly tens to a few hundred
// milliseconds on a modern host; identical across all three languages.
constexpr std::uint64_t kSyscallDefaultIters = 200'000;
constexpr std::uint64_t kMemDefaultPasses = 16;
constexpr std::size_t kMemBufBytes = 128ull * 1024 * 1024; // 128 MiB
constexpr std::uint64_t kIoDefaultIters = 200;
constexpr std::size_t kIoFileBytes = 4096;

[[nodiscard]] std::expected<std::uint64_t, std::string> parse_uint(std::string_view s) {
    if (s.empty()) {
        return std::unexpected("--iters value must not be empty");
    }
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return std::unexpected("--iters value must be a positive integer: " + std::string(s));
        }
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (v == 0) {
        return std::unexpected("--iters value must be >= 1");
    }
    return v;
}

[[nodiscard]] std::expected<Config, std::string> parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--bench") {
            if (++i >= argc) {
                return std::unexpected("--bench requires a value");
            }
            const std::string_view v = argv[i];
            if (v == "syscall") {
                cfg.bench = Bench::Syscall;
            } else if (v == "mem") {
                cfg.bench = Bench::Mem;
            } else if (v == "io") {
                cfg.bench = Bench::Io;
            } else if (v == "all") {
                cfg.bench = Bench::All;
            } else {
                return std::unexpected("unknown --bench value: " + std::string(v));
            }
        } else if (arg == "--iters") {
            if (++i >= argc) {
                return std::unexpected("--iters requires a value");
            }
            auto n = parse_uint(argv[i]);
            if (!n) {
                return std::unexpected(n.error());
            }
            cfg.iters = *n;
        } else {
            return std::unexpected("unknown argument: " + std::string(arg));
        }
    }
    return cfg;
}

// --- syscall bench: tight getpid(2) loop, ns/call. --------------------------
double bench_syscall(std::uint64_t iters) {
    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < iters; ++i) {
        // A raw syscall (not the libc wrapper) so every iteration really
        // crosses into the kernel — this is the syscall-boundary cost the
        // chapter contrasts against VM (vmexit trap) and container
        // (near-zero extra) overhead.
        ::syscall(SYS_getpid);
    }
    const auto elapsed = Clock::now() - start;
    double ns = std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(elapsed).count();
    if (ns <= 0.0) {
        ns = 1.0;
    }
    return ns / static_cast<double>(iters);
}

// --- mem bench: sequential read bandwidth over a big buffer, GB/s. ---------
double bench_mem(std::uint64_t passes) {
    const std::size_t words = kMemBufBytes / sizeof(std::uint64_t);
    std::vector<std::uint64_t> buf(words);
    for (std::size_t i = 0; i < words; ++i) {
        buf[i] = static_cast<std::uint64_t>(i) * 2654435761ull;
    }

    std::uint64_t sum = 0;
    const auto start = Clock::now();
    for (std::uint64_t p = 0; p < passes; ++p) {
        for (std::size_t i = 0; i < words; ++i) {
            sum += buf[i];
        }
    }
    const auto elapsed = Clock::now() - start;
    double secs = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
    if (secs <= 0.0) {
        secs = 1e-9;
    }

    // Consume `sum` after the clock stops so the compiler cannot prove the
    // whole scan is dead and elide it; this doesn't affect the measured time.
    volatile std::uint64_t sink = sum;
    (void)sink;

    const double bytes = static_cast<double>(kMemBufBytes) * static_cast<double>(passes);
    return bytes / secs / 1e9;
}

// --- io bench: create/write/fsync/unlink loop, ops/s. -----------------------
[[nodiscard]] std::expected<double, std::string> bench_io(std::uint64_t iters) {
    // Relative to the CWD (the demo.sh working directory), not /tmp: on many
    // dev hosts /tmp is tmpfs, where fsync is nearly free and the number
    // stops meaning anything as "disk IO overhead". The example directory
    // itself is normally on a real filesystem, which is the point of
    // comparing this number across host/VM/container.
    char dir_template[] = "overheadbench-io-XXXXXX";
    const char* dir = ::mkdtemp(dir_template);
    if (dir == nullptr) {
        return std::unexpected(std::string("mkdtemp: ") + std::strerror(errno));
    }
    const std::string path = std::string(dir) + "/probe";
    const std::vector<char> payload(kIoFileBytes, 'x');

    auto cleanup = [&] {
        ::unlink(path.c_str());
        ::rmdir(dir);
    };

    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < iters; ++i) {
        int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd < 0) {
            cleanup();
            return std::unexpected(std::string("open: ") + std::strerror(errno));
        }
        const ssize_t w = ::write(fd, payload.data(), payload.size());
        if (w != static_cast<ssize_t>(payload.size())) {
            ::close(fd);
            cleanup();
            return std::unexpected("short write");
        }
        if (::fsync(fd) != 0) {
            ::close(fd);
            cleanup();
            return std::unexpected(std::string("fsync: ") + std::strerror(errno));
        }
        ::close(fd);
        ::unlink(path.c_str());
    }
    const auto elapsed = Clock::now() - start;
    double secs = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
    if (secs <= 0.0) {
        secs = 1e-9;
    }
    ::rmdir(dir);
    return static_cast<double>(iters) / secs;
}

struct Row {
    std::string_view name;
    std::string_view unit;
    double value;
};

} // namespace

int main(int argc, char** argv) {
    const auto cfg = parse_args(argc, argv);
    if (!cfg) {
        std::println(stderr, "{}", cfg.error());
        std::println(stderr, "{}", kUsage);
        return 2;
    }

    const bool want_syscall = cfg->bench == Bench::Syscall || cfg->bench == Bench::All;
    const bool want_mem = cfg->bench == Bench::Mem || cfg->bench == Bench::All;
    const bool want_io = cfg->bench == Bench::Io || cfg->bench == Bench::All;

    std::vector<Row> rows;
    if (want_syscall) {
        rows.push_back({"syscall", "ns/call", bench_syscall(cfg->iters.value_or(kSyscallDefaultIters))});
    }
    if (want_mem) {
        rows.push_back({"mem", "GB/s", bench_mem(cfg->iters.value_or(kMemDefaultPasses))});
    }
    if (want_io) {
        auto r = bench_io(cfg->iters.value_or(kIoDefaultIters));
        if (!r) {
            std::println(stderr, "io bench failed: {}", r.error());
            return 1;
        }
        rows.push_back({"io", "ops/s", *r});
    }

    for (const auto& row : rows) {
        std::println("bench={} metric={:.2f} unit={}", row.name, row.value, row.unit);
    }
    return 0;
}
