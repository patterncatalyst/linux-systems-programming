// fwatch v2 — batched sync engine (chapter 10: io_uring).
//
// Subcommands:
//   fwatch scan DIR                                 (v0: polling snapshot)
//   fwatch watch DIR --timeout MS                   (v1: inotify events)
//   fwatch sync SRCDIR DSTDIR [--engine rw|uring]   (v2: batched copy)
//
// The uring engine is a deliberately small, self-contained io_uring layer
// built on raw syscall(2) wrappers: io_uring_setup(2) + io_uring_enter(2),
// with the SQ/CQ rings and the SQE array mapped via mmap(2). No liburing.
// Each file is copied by batching linked READ->WRITE SQE pairs (one pair per
// 128 KiB chunk, up to 8 pairs per io_uring_enter) at explicit offsets.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <print>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/io_uring.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

using std::expected;
using std::unexpected;

[[nodiscard]] std::error_code errno_ec() {
    return std::error_code{errno, std::system_category()};
}

// --- RAII wrappers ---------------------------------------------------------

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    [[nodiscard]] int get() const { return fd_; }

    [[nodiscard]] static expected<Fd, std::error_code> open(const fs::path& path, int flags,
                                                            mode_t mode = 0) {
        const int fd = ::open(path.c_str(), flags, mode);
        if (fd < 0) {
            return unexpected(errno_ec());
        }
        return Fd{fd};
    }

private:
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    int fd_ = -1;
};

class Mmap {
public:
    Mmap() = default;
    ~Mmap() { reset(); }
    Mmap(Mmap&& other) noexcept
        : ptr_(std::exchange(other.ptr_, nullptr)), len_(std::exchange(other.len_, 0)) {}
    Mmap& operator=(Mmap&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = std::exchange(other.ptr_, nullptr);
            len_ = std::exchange(other.len_, 0);
        }
        return *this;
    }
    Mmap(const Mmap&) = delete;
    Mmap& operator=(const Mmap&) = delete;

    [[nodiscard]] static expected<Mmap, std::error_code> map(size_t len, int fd,
                                                             uint64_t offset) {
        void* p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd,
                         static_cast<off_t>(offset));
        if (p == MAP_FAILED) {
            return unexpected(errno_ec());
        }
        Mmap m;
        m.ptr_ = p;
        m.len_ = len;
        return m;
    }

    template <typename T>
    [[nodiscard]] T* at(size_t off) const {
        return reinterpret_cast<T*>(static_cast<std::byte*>(ptr_) + off);
    }

private:
    void reset() {
        if (ptr_ != nullptr) {
            ::munmap(ptr_, len_);
            ptr_ = nullptr;
        }
    }
    void* ptr_ = nullptr;
    size_t len_ = 0;
};

// --- a small educational io_uring layer (no liburing) ----------------------

class Ring {
public:
    [[nodiscard]] static expected<std::unique_ptr<Ring>, std::error_code> create(
        unsigned entries) {
        io_uring_params params{};
        const long fd = ::syscall(__NR_io_uring_setup, entries, &params);  // io_uring_setup(2)
        if (fd < 0) {
            return unexpected(errno_ec());
        }
        auto ring = std::unique_ptr<Ring>(new Ring{});
        ring->fd_ = Fd{static_cast<int>(fd)};

        // The SQ ring holds indices into the separately-mapped SQE array; the
        // CQ ring holds the CQEs themselves. Modern kernels expose both rings
        // through a single mapping (IORING_FEAT_SINGLE_MMAP).
        size_t sq_len = params.sq_off.array + params.sq_entries * sizeof(unsigned);
        size_t cq_len = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
        const bool single_mmap = (params.features & IORING_FEAT_SINGLE_MMAP) != 0;
        if (single_mmap) {
            sq_len = cq_len = std::max(sq_len, cq_len);
        }

        auto sq_map = Mmap::map(sq_len, ring->fd_.get(), IORING_OFF_SQ_RING);
        if (!sq_map) {
            return unexpected(sq_map.error());
        }
        ring->sq_map_ = std::move(*sq_map);

        const Mmap* cq_view = &ring->sq_map_;
        if (!single_mmap) {
            auto cq_map = Mmap::map(cq_len, ring->fd_.get(), IORING_OFF_CQ_RING);
            if (!cq_map) {
                return unexpected(cq_map.error());
            }
            ring->cq_map_ = std::move(*cq_map);
            cq_view = &ring->cq_map_;
        }

        auto sqe_map = Mmap::map(params.sq_entries * sizeof(io_uring_sqe), ring->fd_.get(),
                                 IORING_OFF_SQES);
        if (!sqe_map) {
            return unexpected(sqe_map.error());
        }
        ring->sqe_map_ = std::move(*sqe_map);

        ring->sq_head_ = ring->sq_map_.at<unsigned>(params.sq_off.head);
        ring->sq_tail_ = ring->sq_map_.at<unsigned>(params.sq_off.tail);
        ring->sq_mask_ = *ring->sq_map_.at<unsigned>(params.sq_off.ring_mask);
        ring->sq_array_ = ring->sq_map_.at<unsigned>(params.sq_off.array);
        ring->cq_head_ = cq_view->at<unsigned>(params.cq_off.head);
        ring->cq_tail_ = cq_view->at<unsigned>(params.cq_off.tail);
        ring->cq_mask_ = *cq_view->at<unsigned>(params.cq_off.ring_mask);
        ring->cqes_ = cq_view->at<io_uring_cqe>(params.cq_off.cqes);
        ring->sqes_ = ring->sqe_map_.at<io_uring_sqe>(0);
        ring->local_tail_ = *ring->sq_tail_;
        return ring;
    }

    // Claim the next SQE slot (zeroed); nullptr when the SQ ring is full.
    [[nodiscard]] io_uring_sqe* try_get_sqe() {
        const unsigned head = std::atomic_ref(*sq_head_).load(std::memory_order_acquire);
        if (local_tail_ - head > sq_mask_) {
            return nullptr;
        }
        const unsigned idx = local_tail_ & sq_mask_;
        io_uring_sqe* sqe = &sqes_[idx];
        std::memset(sqe, 0, sizeof(*sqe));
        sq_array_[idx] = idx;
        ++local_tail_;
        ++to_submit_;
        return sqe;
    }

    // Publish queued SQEs and wait for wait_nr completions in one syscall.
    [[nodiscard]] expected<void, std::error_code> submit_and_wait(unsigned wait_nr) {
        std::atomic_ref(*sq_tail_).store(local_tail_, std::memory_order_release);
        return enter(std::exchange(to_submit_, 0), wait_nr);
    }

    // Pop one CQE if available.
    [[nodiscard]] bool pop(io_uring_cqe& out) {
        const unsigned head = *cq_head_;
        const unsigned tail = std::atomic_ref(*cq_tail_).load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        out = cqes_[head & cq_mask_];
        std::atomic_ref(*cq_head_).store(head + 1, std::memory_order_release);
        return true;
    }

    // Consume exactly `need` CQEs, re-entering the kernel if a wait returned
    // before all completions had arrived.
    template <typename OnCqe>
    [[nodiscard]] expected<void, std::error_code> drain(unsigned need, OnCqe&& on_cqe) {
        io_uring_cqe cqe{};
        while (need > 0) {
            while (need > 0 && pop(cqe)) {
                on_cqe(cqe);
                --need;
            }
            if (need == 0) {
                break;
            }
            if (auto r = enter(0, need); !r) {
                return unexpected(r.error());
            }
        }
        return {};
    }

private:
    Ring() = default;

    [[nodiscard]] expected<void, std::error_code> enter(unsigned to_submit, unsigned wait_nr) {
        while (true) {
            const long r = ::syscall(__NR_io_uring_enter, fd_.get(), to_submit, wait_nr,
                                     IORING_ENTER_GETEVENTS, nullptr, 0);  // io_uring_enter(2)
            if (r >= 0) {
                return {};
            }
            if (errno == EINTR) {
                continue;  // safe: the kernel caps submission at what is queued
            }
            return unexpected(errno_ec());
        }
    }

    Fd fd_;
    Mmap sq_map_;
    Mmap cq_map_;  // unused when IORING_FEAT_SINGLE_MMAP
    Mmap sqe_map_;
    unsigned* sq_head_ = nullptr;
    unsigned* sq_tail_ = nullptr;
    unsigned* sq_array_ = nullptr;
    unsigned* cq_head_ = nullptr;
    unsigned* cq_tail_ = nullptr;
    io_uring_sqe* sqes_ = nullptr;
    io_uring_cqe* cqes_ = nullptr;
    unsigned sq_mask_ = 0;
    unsigned cq_mask_ = 0;
    unsigned local_tail_ = 0;
    unsigned to_submit_ = 0;
};

// --- copy engines ----------------------------------------------------------

constexpr size_t kChunk = 128 * 1024;
constexpr unsigned kPairs = 8;  // READ->WRITE pairs batched per submit

[[nodiscard]] expected<uint64_t, std::error_code> copy_file_rw(const fs::path& src,
                                                               const fs::path& dst) {
    auto in = Fd::open(src, O_RDONLY | O_CLOEXEC);
    if (!in) {
        return unexpected(in.error());
    }
    auto out = Fd::open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (!out) {
        return unexpected(out.error());
    }
    std::vector<char> buf(kChunk);
    uint64_t total = 0;
    while (true) {
        const ssize_t n = ::read(in->get(), buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return unexpected(errno_ec());
        }
        if (n == 0) {
            break;
        }
        size_t written = 0;
        while (written < static_cast<size_t>(n)) {
            const ssize_t w = ::write(out->get(), buf.data() + written,
                                      static_cast<size_t>(n) - written);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return unexpected(errno_ec());
            }
            written += static_cast<size_t>(w);
        }
        total += static_cast<uint64_t>(n);
    }
    return total;
}

[[nodiscard]] expected<uint64_t, std::error_code> copy_file_uring(Ring& ring, const fs::path& src,
                                                                  const fs::path& dst) {
    auto in = Fd::open(src, O_RDONLY | O_CLOEXEC);
    if (!in) {
        return unexpected(in.error());
    }
    auto out = Fd::open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (!out) {
        return unexpected(out.error());
    }
    struct stat st{};
    if (::fstat(in->get(), &st) != 0) {
        return unexpected(errno_ec());
    }
    const auto size = static_cast<uint64_t>(st.st_size);

    std::array<std::vector<char>, kPairs> bufs;
    for (auto& b : bufs) {
        b.resize(kChunk);
    }

    uint64_t off = 0;
    while (off < size) {
        std::array<unsigned, kPairs> lens{};
        unsigned pairs = 0;
        while (pairs < kPairs && off < size) {
            const auto len = static_cast<unsigned>(std::min<uint64_t>(kChunk, size - off));
            lens[pairs] = len;
            io_uring_sqe* rd = ring.try_get_sqe();
            io_uring_sqe* wr = rd != nullptr ? ring.try_get_sqe() : nullptr;
            if (rd == nullptr || wr == nullptr) {
                return unexpected(std::make_error_code(std::errc::no_buffer_space));
            }
            // READ this chunk into the pair's buffer...
            rd->opcode = IORING_OP_READ;
            rd->fd = in->get();
            rd->addr = reinterpret_cast<uint64_t>(bufs[pairs].data());
            rd->len = len;
            rd->off = off;
            rd->flags = IOSQE_IO_LINK;  // ...then, only if it fully succeeded,
            rd->user_data = uint64_t{pairs} << 1U;
            // ...WRITE the same buffer at the same offset in dst.
            wr->opcode = IORING_OP_WRITE;
            wr->fd = out->get();
            wr->addr = reinterpret_cast<uint64_t>(bufs[pairs].data());
            wr->len = len;
            wr->off = off;
            wr->user_data = (uint64_t{pairs} << 1U) | 1U;
            off += len;
            ++pairs;
        }
        if (auto s = ring.submit_and_wait(2 * pairs); !s) {
            return unexpected(s.error());
        }
        bool short_io = false;
        auto d = ring.drain(2 * pairs, [&](const io_uring_cqe& cqe) {
            const auto slot = static_cast<unsigned>(cqe.user_data >> 1U);
            if (cqe.res < 0 || static_cast<unsigned>(cqe.res) != lens[slot]) {
                short_io = true;
            }
        });
        if (!d) {
            return unexpected(d.error());
        }
        if (short_io) {
            return unexpected(std::make_error_code(std::errc::io_error));
        }
    }
    return size;
}

// --- subcommands -----------------------------------------------------------

int usage() {
    std::println(stderr, "usage: fwatch <command>");
    std::println(stderr, "  fwatch scan DIR");
    std::println(stderr, "  fwatch watch DIR --timeout MS");
    std::println(stderr, "  fwatch sync SRCDIR DSTDIR [--engine rw|uring]");
    return 2;
}

int fail(std::string_view what, std::error_code ec) {
    std::println(stderr, "fwatch: {}: {}", what, ec.message());
    return 1;
}

int cmd_scan(const fs::path& dir) {
    std::error_code ec;
    uint64_t files = 0;
    uint64_t bytes = 0;
    fs::recursive_directory_iterator it(dir, fs::directory_options::none, ec);
    if (ec) {
        return fail(dir.string(), ec);
    }
    for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            return fail(dir.string(), ec);
        }
        const auto st = it->symlink_status(ec);
        if (ec) {
            return fail(it->path().string(), ec);
        }
        if (fs::is_regular_file(st)) {
            const auto sz = fs::file_size(it->path(), ec);
            if (ec) {
                return fail(it->path().string(), ec);
            }
            ++files;
            bytes += sz;
        }
    }
    std::println("scanned {} files {} bytes", files, bytes);
    return 0;
}

int cmd_watch(const fs::path& dir, long timeout_ms) {
    Fd ifd{::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)};
    if (ifd.get() < 0) {
        return fail("inotify_init1", errno_ec());
    }
    if (::inotify_add_watch(ifd.get(), dir.c_str(), IN_CREATE | IN_MODIFY | IN_DELETE) < 0) {
        return fail(dir.string(), errno_ec());
    }
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    uint64_t events = 0;
    alignas(inotify_event) std::array<char, 16 * 1024> buf{};
    while (true) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now())
                .count();
        if (remaining <= 0) {
            break;
        }
        pollfd pfd{.fd = ifd.get(), .events = POLLIN, .revents = 0};
        const int rc = ::poll(&pfd, 1, static_cast<int>(remaining));
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail("poll", errno_ec());
        }
        if (rc == 0) {
            continue;  // deadline check at the loop top ends the watch
        }
        const ssize_t n = ::read(ifd.get(), buf.data(), buf.size());
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            return fail("inotify read", errno_ec());
        }
        for (ssize_t pos = 0; pos < n;) {
            const auto* ev = reinterpret_cast<const inotify_event*>(buf.data() + pos);
            std::string_view kind;
            if ((ev->mask & IN_CREATE) != 0) {
                kind = "CREATE";
            } else if ((ev->mask & IN_MODIFY) != 0) {
                kind = "MODIFY";
            } else if ((ev->mask & IN_DELETE) != 0) {
                kind = "DELETE";
            }
            if (!kind.empty() && ev->len > 0) {
                std::println("event {} {}", kind, static_cast<const char*>(ev->name));
                ++events;
            }
            pos += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
        }
    }
    std::println("watched {} events", events);
    return 0;
}

int cmd_sync(const fs::path& src, const fs::path& dst, std::string_view engine) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    std::error_code ec;
    if (!fs::is_directory(src, ec) || ec) {
        return fail(src.string(), std::make_error_code(std::errc::not_a_directory));
    }
    fs::create_directories(dst, ec);
    if (ec) {
        return fail(dst.string(), ec);
    }

    std::unique_ptr<Ring> ring;
    if (engine == "uring") {
        auto r = Ring::create(64);
        if (!r) {
            return fail("io_uring_setup", r.error());
        }
        ring = std::move(*r);
    }

    uint64_t files = 0;
    uint64_t bytes = 0;
    fs::recursive_directory_iterator it(src, fs::directory_options::none, ec);
    if (ec) {
        return fail(src.string(), ec);
    }
    for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            return fail(src.string(), ec);
        }
        const fs::path target = dst / it->path().lexically_relative(src);
        const auto st = it->symlink_status(ec);
        if (ec) {
            return fail(it->path().string(), ec);
        }
        if (fs::is_directory(st)) {
            fs::create_directories(target, ec);
            if (ec) {
                return fail(target.string(), ec);
            }
        } else if (fs::is_regular_file(st)) {
            const auto copied = ring != nullptr ? copy_file_uring(*ring, it->path(), target)
                                                : copy_file_rw(it->path(), target);
            if (!copied) {
                return fail(it->path().string(), copied.error());
            }
            ++files;
            bytes += *copied;
        }
    }

    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
    std::println("synced {} files {} bytes engine={} ms={}", files, bytes, engine, ms);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) {
        return usage();
    }
    const std::string_view cmd = args[0];

    if (cmd == "scan" && args.size() == 2) {
        return cmd_scan(args[1]);
    }
    if (cmd == "watch" && args.size() == 4 && args[2] == "--timeout") {
        long timeout_ms = 0;
        const auto [ptr, err] =
            std::from_chars(args[3].data(), args[3].data() + args[3].size(), timeout_ms);
        if (err != std::errc{} || ptr != args[3].data() + args[3].size() || timeout_ms < 0) {
            return usage();
        }
        return cmd_watch(args[1], timeout_ms);
    }
    if (cmd == "sync" && (args.size() == 3 || args.size() == 5)) {
        std::string_view engine = "rw";
        if (args.size() == 5) {
            if (args[3] != "--engine") {
                return usage();
            }
            engine = args[4];
        }
        if (engine != "rw" && engine != "uring") {
            return usage();
        }
        return cmd_sync(args[1], args[2], engine);
    }
    return usage();
}
