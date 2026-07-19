// iobench: page cache vs durability — buffered writes, periodic fdatasync(2),
// and O_DIRECT, with identical CLI and output across the three languages.
//
//   iobench --mode buffered|fsync-every|direct [--every N] [--size-mb M] FILE
//
// Writes M MiB (default 64) in 64 KiB blocks.
//   buffered     plain write(2)s; reports write time, then a second line
//                "fsync_ms=<t>" for the closing fdatasync(2)
//   fsync-every  fdatasync(2) every N blocks (default 8), timed end to end
//   direct       O_DIRECT with 4096-aligned buffers; if open(2) fails with
//                EINVAL, prints "direct: unsupported on this filesystem"
//                and exits 4

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr std::size_t kBlockSize = 64 * 1024;
constexpr std::size_t kAlignment = 4096;
constexpr std::size_t kMiB = 1024 * 1024;

enum class Mode { buffered, fsync_every, direct };

[[nodiscard]] constexpr std::string_view mode_name(Mode m) {
    switch (m) {
    case Mode::buffered:    return "buffered";
    case Mode::fsync_every: return "fsync-every";
    case Mode::direct:      return "direct";
    }
    return "?";
}

struct Options {
    Mode mode = Mode::buffered;
    std::size_t every = 8;
    std::size_t size_mb = 64;
    std::string file;
};

// Owning RAII wrapper around a file descriptor.
class Fd {
public:
    explicit Fd(int fd) noexcept : fd_{fd} {}
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

    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    int fd_ = -1;
};

[[nodiscard]] std::error_code last_error() {
    return {errno, std::system_category()};
}

[[nodiscard]] std::expected<Fd, std::error_code> open_output(const std::string& path, bool direct) {
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (direct) {
        flags |= O_DIRECT;
    }
    const int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        return std::unexpected(last_error());
    }
    return Fd{fd};
}

[[nodiscard]] std::expected<void, std::error_code> write_all(const Fd& fd,
                                                            std::span<const std::byte> data) {
    while (!data.empty()) {
        const ssize_t n = ::write(fd.get(), data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(last_error());
        }
        data = data.subspan(static_cast<std::size_t>(n));
    }
    return {};
}

[[nodiscard]] std::expected<void, std::error_code> datasync(const Fd& fd) {
    if (::fdatasync(fd.get()) != 0) {
        return std::unexpected(last_error());
    }
    return {};
}

struct FreeDeleter {
    void operator()(void* p) const noexcept { std::free(p); }
};
using AlignedBlock = std::unique_ptr<std::byte[], FreeDeleter>;

// One 64 KiB block, 4096-aligned (required by O_DIRECT, harmless otherwise).
[[nodiscard]] AlignedBlock make_block() {
    auto* raw = static_cast<std::byte*>(std::aligned_alloc(kAlignment, kBlockSize));
    AlignedBlock block{raw};
    if (block) {
        for (std::size_t i = 0; i < kBlockSize; ++i) {
            block[i] = static_cast<std::byte>(i & 0xFF);
        }
    }
    return block;
}

[[nodiscard]] std::optional<std::size_t> parse_positive(std::string_view text) {
    std::size_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value == 0) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<Options> parse_args(std::span<char*> args) {
    Options opt;
    bool have_mode = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "--mode" || a == "--every" || a == "--size-mb") {
            if (i + 1 >= args.size()) {
                return std::nullopt;
            }
            const std::string_view v = args[++i];
            if (a == "--mode") {
                if (v == "buffered") {
                    opt.mode = Mode::buffered;
                } else if (v == "fsync-every") {
                    opt.mode = Mode::fsync_every;
                } else if (v == "direct") {
                    opt.mode = Mode::direct;
                } else {
                    return std::nullopt;
                }
                have_mode = true;
            } else {
                const auto n = parse_positive(v);
                if (!n) {
                    return std::nullopt;
                }
                (a == "--every" ? opt.every : opt.size_mb) = *n;
            }
        } else if (a.starts_with("--")) {
            return std::nullopt;
        } else if (opt.file.empty()) {
            opt.file = a;
        } else {
            return std::nullopt;
        }
    }
    if (!have_mode || opt.file.empty()) {
        return std::nullopt;
    }
    return opt;
}

// Elapsed nanoseconds, clamped to >= 1 so the throughput division is defined.
[[nodiscard]] std::int64_t elapsed_ns(std::chrono::steady_clock::time_point from,
                                      std::chrono::steady_clock::time_point to) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count();
    return ns > 0 ? ns : 1;
}

[[nodiscard]] int run(const Options& opt) {
    auto fd = open_output(opt.file, opt.mode == Mode::direct);
    if (!fd) {
        if (opt.mode == Mode::direct && fd.error() == std::errc::invalid_argument) {
            std::println(stderr, "direct: unsupported on this filesystem");
            return 4;
        }
        std::println(stderr, "error: open {}: {}", opt.file, fd.error().message());
        return 1;
    }

    const auto block = make_block();
    if (!block) {
        std::println(stderr, "error: aligned_alloc failed");
        return 1;
    }
    const std::span<const std::byte> data{block.get(), kBlockSize};

    const std::size_t nblocks = opt.size_mb * (kMiB / kBlockSize);
    const auto bytes = static_cast<std::uint64_t>(opt.size_mb) * kMiB;

    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < nblocks; ++i) {
        if (const auto w = write_all(*fd, data); !w) {
            std::println(stderr, "error: write {}: {}", opt.file, w.error().message());
            return 1;
        }
        if (opt.mode == Mode::fsync_every && (i + 1) % opt.every == 0) {
            if (const auto s = datasync(*fd); !s) {
                std::println(stderr, "error: fdatasync {}: {}", opt.file, s.error().message());
                return 1;
            }
        }
    }
    if (opt.mode == Mode::fsync_every && nblocks % opt.every != 0) {
        if (const auto s = datasync(*fd); !s) {
            std::println(stderr, "error: fdatasync {}: {}", opt.file, s.error().message());
            return 1;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();

    const std::int64_t ns = elapsed_ns(t0, t1);
    const double mib_per_s =
        (static_cast<double>(bytes) / static_cast<double>(kMiB)) / (static_cast<double>(ns) / 1e9);
    std::println("mode={} bytes={} ms={} MiB/s={:.1f}", mode_name(opt.mode), bytes,
                 ns / 1'000'000, mib_per_s);

    if (opt.mode == Mode::buffered) {
        const auto t2 = std::chrono::steady_clock::now();
        if (const auto s = datasync(*fd); !s) {
            std::println(stderr, "error: fdatasync {}: {}", opt.file, s.error().message());
            return 1;
        }
        const auto t3 = std::chrono::steady_clock::now();
        std::println("fsync_ms={}", elapsed_ns(t2, t3) / 1'000'000);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const auto opt = parse_args(std::span{argv + 1, static_cast<std::size_t>(argc - 1)});
    if (!opt) {
        std::println(stderr,
                     "usage: iobench --mode buffered|fsync-every|direct [--every N] [--size-mb M] FILE");
        return 2;
    }
    return run(*opt);
}
