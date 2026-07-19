// sysprobe: a labeled syscall specimen — openat+write+close of an unlinked
// temp file, a 10 ms nanosleep, and 16 bytes of getrandom — built to be
// watched under strace(1). Prints "step=<name> ok" per step, then a summary.

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

namespace {

[[nodiscard]] std::error_code last_error() {
    return {errno, std::system_category()};
}

// Owning RAII wrapper for a file descriptor; the destructor issues close(2).
class unique_fd {
public:
    unique_fd() = default;
    explicit unique_fd(int fd) : fd_(fd) {}
    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;
    unique_fd(unique_fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    unique_fd& operator=(unique_fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~unique_fd() { reset(); }

    [[nodiscard]] int get() const { return fd_; }
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

// Step 1: openat(2). Prefer an anonymous O_TMPFILE inode (no name ever
// appears in the directory); fall back to mkstemp(3) + unlink(2) on
// filesystems that lack O_TMPFILE.
[[nodiscard]] std::expected<unique_fd, std::error_code>
open_scratch(const std::string& dir) {
    if (const int fd =
            ::openat(AT_FDCWD, dir.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
        fd >= 0) {
        return unique_fd{fd};
    }
    if (errno != EOPNOTSUPP && errno != EISDIR && errno != EINVAL) {
        return std::unexpected(last_error());
    }
    std::string tmpl = dir + "/sysprobe.XXXXXX";
    const int fd = ::mkstemp(tmpl.data());
    if (fd < 0) {
        return std::unexpected(last_error());
    }
    unique_fd owned{fd};
    if (::unlink(tmpl.c_str()) != 0) {
        return std::unexpected(last_error());
    }
    return owned;
}

// Step 2: write(2), retrying on EINTR and short writes.
[[nodiscard]] std::expected<void, std::error_code>
write_all(const unique_fd& fd, std::span<const char> buf) {
    while (!buf.empty()) {
        const ssize_t n = ::write(fd.get(), buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(last_error());
        }
        buf = buf.subspan(static_cast<std::size_t>(n));
    }
    return {};
}

// Step 3: nanosleep(2), resuming with the remaining time on EINTR.
[[nodiscard]] std::expected<void, std::error_code> sleep_ms(long ms) {
    timespec req{.tv_sec = 0, .tv_nsec = ms * 1'000'000L};
    timespec rem{};
    while (::nanosleep(&req, &rem) != 0) {
        if (errno != EINTR) {
            return std::unexpected(last_error());
        }
        req = rem;
    }
    return {};
}

// Step 4: getrandom(2), looping over partial reads.
[[nodiscard]] std::expected<void, std::error_code>
fill_random(std::span<std::byte> buf) {
    while (!buf.empty()) {
        const ssize_t n = ::getrandom(buf.data(), buf.size(), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(last_error());
        }
        buf = buf.subspan(static_cast<std::size_t>(n));
    }
    return {};
}

} // namespace

int main(int argc, char**) {
    if (argc > 1) {
        std::println(stderr, "usage: app");
        return 2;
    }

    const char* env = std::getenv("TMPDIR");
    const std::string dir = (env != nullptr && *env != '\0') ? env : "/tmp";

    {
        auto fd = open_scratch(dir);
        if (!fd) {
            std::println(stderr, "sysprobe: open: {}", fd.error().message());
            return 1;
        }
        std::println("step=open ok");

        constexpr std::string_view payload = "sysprobe scratch payload\n";
        if (auto w = write_all(*fd, std::span{payload.data(), payload.size()}); !w) {
            std::println(stderr, "sysprobe: write: {}", w.error().message());
            return 1;
        }
        std::println("step=write ok");
    } // unique_fd destructor issues close(2) here, before the sleep

    if (auto s = sleep_ms(10); !s) {
        std::println(stderr, "sysprobe: sleep: {}", s.error().message());
        return 1;
    }
    std::println("step=sleep ok");

    std::array<std::byte, 16> entropy{};
    if (auto r = fill_random(entropy); !r) {
        std::println(stderr, "sysprobe: random: {}", r.error().message());
        return 1;
    }
    std::println("step=random ok");

    std::println("sysprobe: 4 steps ok");
    return 0;
}
