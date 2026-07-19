// copyx SRC DST — file copier that establishes the book's error taxonomy.
//
//   exit 0 — success; "copied <N> bytes" on stdout
//   exit 2 — source-side failure (open/read SRC); "copyx: <reason>" on stderr
//   exit 3 — destination-side failure (open/write/close DST); same shape
//
// EINTR on read(2)/write(2) is retried (the syscall was interrupted before
// completing; reissuing it is the only correct policy), and short writes are
// resumed until the whole buffer is on its way.

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <print>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace {

[[nodiscard]] std::error_code last_error() noexcept {
    return {errno, std::generic_category()};
}

// RAII owner of a file descriptor: closes on scope exit, move-only.
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

    // Explicit close for callers that must observe the result: on the write
    // side, close(2) is where deferred IO errors surface.
    [[nodiscard]] std::expected<void, std::error_code> close() noexcept {
        if (fd_ < 0) {
            return {};
        }
        if (::close(std::exchange(fd_, -1)) != 0) {
            return std::unexpected(last_error());
        }
        return {};
    }

private:
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(std::exchange(fd_, -1));
        }
    }
    int fd_ = -1;
};

[[nodiscard]] std::expected<Fd, std::error_code>
open_fd(const char* path, int flags, mode_t mode = 0) {
    const int fd = ::open(path, flags, mode); // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (fd < 0) {
        return std::unexpected(last_error());
    }
    return Fd{fd};
}

// read(2) once, retrying EINTR: interrupted means nothing was consumed.
[[nodiscard]] std::expected<std::size_t, std::error_code>
read_some(const Fd& fd, std::span<char> buf) {
    for (;;) {
        const ssize_t n = ::read(fd.get(), buf.data(), buf.size());
        if (n >= 0) {
            return static_cast<std::size_t>(n);
        }
        if (errno == EINTR) {
            continue; // interrupted before transferring anything: retry
        }
        return std::unexpected(last_error());
    }
}

// write(2) until the whole span is written: EINTR restarts the call, a short
// write resumes from where the kernel stopped.
[[nodiscard]] std::expected<void, std::error_code>
write_all(const Fd& fd, std::span<const char> buf) {
    while (!buf.empty()) {
        const ssize_t n = ::write(fd.get(), buf.data(), buf.size());
        if (n > 0) {
            buf = buf.subspan(static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue; // interrupted: reissue the same span
        }
        return std::unexpected(last_error());
    }
    return {};
}

struct Failure {
    std::string message; // printed after the "copyx: " prefix
    int exit_code;
};

[[nodiscard]] std::expected<std::uint64_t, Failure>
copy_file(const char* src_path, const char* dst_path) {
    auto src = open_fd(src_path, O_RDONLY | O_CLOEXEC);
    if (!src) {
        return std::unexpected(
            Failure{std::format("{}: {}", src_path, src.error().message()), 2});
    }

    auto dst = open_fd(dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (!dst) {
        return std::unexpected(
            Failure{std::format("{}: {}", dst_path, dst.error().message()), 3});
    }

    std::uint64_t total = 0;
    std::array<char, 64 * 1024> buf{};
    for (;;) {
        const auto n = read_some(*src, buf);
        if (!n) {
            return std::unexpected(
                Failure{std::format("read {}: {}", src_path, n.error().message()), 2});
        }
        if (*n == 0) {
            break; // EOF
        }
        if (auto w = write_all(*dst, std::span<const char>{buf}.first(*n)); !w) {
            return std::unexpected(
                Failure{std::format("write {}: {}", dst_path, w.error().message()), 3});
        }
        total += *n;
    }

    // The write side must observe close(2): deferred IO errors land here.
    if (auto c = dst->close(); !c) {
        return std::unexpected(
            Failure{std::format("close {}: {}", dst_path, c.error().message()), 3});
    }
    return total;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::println(stderr, "usage: copyx SRC DST");
        return 2;
    }
    const auto copied = copy_file(argv[1], argv[2]);
    if (!copied) {
        std::println(stderr, "copyx: {}", copied.error().message);
        return copied.error().exit_code;
    }
    std::println("copied {} bytes", *copied);
    return 0;
}
