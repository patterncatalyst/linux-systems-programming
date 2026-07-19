// parhash: parallel FNV-1a 64 checksummer — C++23 flavor.
//
// Concurrency shape: 4 std::jthread workers pull paths from a mutex-guarded
// queue (condition_variable_any so waits are stop_token-aware). A dedicated
// jthread sigtimedwait()s for SIGINT (blocked in every other thread) and, on
// delivery, flips an atomic flag and request_stop()s the pool: the producer
// stops enqueuing, workers finish the file they are on and exit, and main
// prints whatever completed plus "parhash: interrupted" (exit 130).

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <print>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr int kWorkers = 4;
constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x00000100000001b3ULL;

// RAII wrapper: an owned file descriptor, closed exactly once.
class OwnedFd {
public:
    explicit OwnedFd(int fd) noexcept : fd_(fd) {}
    ~OwnedFd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;
    OwnedFd(OwnedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    OwnedFd& operator=(OwnedFd&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_;
};

[[nodiscard]] std::error_code last_error() {
    return {errno, std::system_category()};
}

// Stream a file through FNV-1a 64. Never throws; errors come back typed.
[[nodiscard]] std::expected<std::uint64_t, std::error_code> hash_file(const fs::path& path) {
    OwnedFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd.valid()) {
        return std::unexpected(last_error());
    }
    std::uint64_t hash = kFnvOffset;
    std::array<unsigned char, 64 * 1024> buf{};
    for (;;) {
        const ssize_t n = ::read(fd.get(), buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(last_error());
        }
        if (n == 0) {
            break;
        }
        for (ssize_t i = 0; i < n; ++i) {
            hash ^= buf[static_cast<std::size_t>(i)];
            hash *= kFnvPrime;
        }
    }
    return hash;
}

// Work item: path relative to the root (display + sort key) plus the real path.
struct Task {
    std::string rel;
    fs::path full;
};

// Mutex-guarded FIFO. pop() blocks with a stop_token-aware wait, so a
// request_stop() wakes idle workers immediately and makes them refuse
// queued-but-unstarted items; a file a worker is mid-hash on always finishes.
class TaskQueue {
public:
    void push(Task task) {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] std::optional<Task> pop(std::stop_token st) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, st, [this] { return closed_ || !queue_.empty(); });
        if (st.stop_requested() || queue_.empty()) {
            return std::nullopt;
        }
        Task task = std::move(queue_.front());
        queue_.pop_front();
        return task;
    }

private:
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<Task> queue_;
    bool closed_ = false;
};

struct Entry {
    std::string rel;
    std::uint64_t hash;
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::println(stderr, "usage: parhash DIR");
        return 2;
    }
    const fs::path root = argv[1];
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        std::println(stderr, "parhash: cannot walk {}", root.string());
        return 1;
    }

    // Block SIGINT process-wide before any thread exists; only the watcher
    // thread consumes it, via sigtimedwait, so no async handler ever runs.
    sigset_t sigint_set{};
    sigemptyset(&sigint_set);
    sigaddset(&sigint_set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &sigint_set, nullptr);

    std::atomic<bool> interrupted{false};
    std::stop_source cancel;
    TaskQueue queue;

    std::mutex results_mutex;
    std::vector<Entry> results;

    std::jthread watcher([&](std::stop_token st) {
        const timespec tick{.tv_sec = 0, .tv_nsec = 20'000'000}; // 20 ms poll
        while (!st.stop_requested()) {
            if (::sigtimedwait(&sigint_set, nullptr, &tick) == SIGINT) {
                interrupted.store(true, std::memory_order_relaxed);
                cancel.request_stop(); // stop accepting work; drain in-flight
                return;
            }
        }
    });

    {
        std::vector<std::jthread> workers;
        workers.reserve(kWorkers);
        for (int i = 0; i < kWorkers; ++i) {
            workers.emplace_back([&, st = cancel.get_token()] {
                while (auto task = queue.pop(st)) {
                    const auto hash = hash_file(task->full);
                    if (!hash) {
                        std::println(stderr, "parhash: skipping {}", task->rel);
                        continue;
                    }
                    std::lock_guard lock(results_mutex);
                    results.push_back({std::move(task->rel), *hash});
                }
            });
        }

        // Producer: stream the walk into the queue, checking for cancellation
        // between entries so a big tree stops enumerating promptly.
        for (auto it = fs::recursive_directory_iterator(root, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (interrupted.load(std::memory_order_relaxed)) {
                break;
            }
            std::error_code type_ec;
            // symlink_status: classify the entry itself, never what it links to
            // (matches Go's DirEntry.Type and Rust's DirEntry::file_type).
            if (!fs::is_regular_file(it->symlink_status(type_ec)) || type_ec) {
                continue;
            }
            queue.push({it->path().lexically_relative(root).generic_string(), it->path()});
        }
        queue.close();
    } // jthread dtors join all four workers here

    watcher.request_stop();
    watcher.join();

    std::ranges::sort(results, {}, &Entry::rel);
    for (const auto& entry : results) {
        std::println("{:016x}  {}", entry.hash, entry.rel);
    }
    if (interrupted.load(std::memory_order_relaxed)) {
        std::println(stderr, "parhash: interrupted");
        return 130;
    }
    std::println("parhash: {} files, {} workers", results.size(), kWorkers);
    return 0;
}
