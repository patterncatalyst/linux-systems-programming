// fwatch v0 — snapshot/diff a directory tree with dirfd-relative syscalls.
//
//   fwatch snapshot DIR                one "<relpath> <size> <mtime>" line per
//                                      regular file, sorted by path
//   fwatch diff DIR SNAPSHOT_FILE      re-scan and print +/-/~ lines plus a
//                                      "fwatch: A added, R removed, M modified"
//                                      summary
//
// The walk is deliberately explicit: openat(2) relative to the parent
// directory fd, fdopendir(3) to enumerate, fstatat(2) with
// AT_SYMLINK_NOFOLLOW to classify. Every fd and DIR* lives inside an RAII
// wrapper; fallible operations return std::expected.

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <print>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// --- RAII wrappers -----------------------------------------------------------

// Owns a file descriptor; closes it exactly once.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd() { reset(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
    explicit operator bool() const noexcept { return fd_ >= 0; }

private:
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    int fd_ = -1;
};

// Owns a DIR*. fdopendir(3) transfers ownership of the underlying fd to the
// DIR stream, so closedir(3) is the one and only cleanup.
class DirStream {
public:
    // Adopt an already-open directory fd. On success the Fd is released into
    // the DIR*; on failure the Fd destructor still closes it.
    [[nodiscard]] static std::expected<DirStream, std::error_code> adopt(Fd fd) {
        DIR* dir = ::fdopendir(fd.get());
        if (dir == nullptr) {
            return std::unexpected(std::error_code{errno, std::system_category()});
        }
        (void)fd.release(); // the DIR* owns the fd now
        return DirStream{dir};
    }

    ~DirStream() {
        if (dir_ != nullptr) ::closedir(dir_);
    }

    DirStream(const DirStream&) = delete;
    DirStream& operator=(const DirStream&) = delete;

    DirStream(DirStream&& other) noexcept : dir_(std::exchange(other.dir_, nullptr)) {}
    DirStream& operator=(DirStream&& other) noexcept {
        if (this != &other) {
            if (dir_ != nullptr) ::closedir(dir_);
            dir_ = std::exchange(other.dir_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] DIR* get() const noexcept { return dir_; }
    [[nodiscard]] int fd() const noexcept { return ::dirfd(dir_); }

private:
    explicit DirStream(DIR* dir) noexcept : dir_(dir) {}
    DIR* dir_ = nullptr;
};

// --- Scanning ----------------------------------------------------------------

struct Info {
    std::int64_t size;
    std::int64_t mtime;
    bool operator==(const Info&) const = default;
};

using Tree = std::map<std::string, Info>; // sorted by path, bytewise

// A failed operation, already formatted with the path it failed on.
struct Failure {
    std::string message; // e.g. "cannot open directory 'x' (errno 2)"
};

constexpr int kDirFlags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;

[[nodiscard]] std::expected<Fd, std::error_code> open_dir_at(int parent_fd,
                                                             const char* name) {
    const int raw = ::openat(parent_fd, name, kDirFlags);
    if (raw < 0) {
        return std::unexpected(std::error_code{errno, std::system_category()});
    }
    return Fd{raw};
}

[[nodiscard]] std::expected<struct stat, std::error_code> stat_at(int dirfd,
                                                                  const char* name) {
    struct stat st{};
    if (::fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return std::unexpected(std::error_code{errno, std::system_category()});
    }
    return st;
}

[[nodiscard]] std::string join(const std::string& base, const std::string& name) {
    if (base.empty()) return name;
    return base + "/" + name;
}

[[nodiscard]] Failure fail(const std::string& what, const std::string& path,
                           std::error_code ec) {
    return Failure{std::format("cannot {} '{}' (errno {})", what, path, ec.value())};
}

// Walk the directory opened as `fd`, recording every regular file into `tree`.
// `display` is the path for error messages, `rel` the DIR-relative prefix.
[[nodiscard]] std::expected<void, Failure> walk(Fd fd, const std::string& display,
                                                const std::string& rel, Tree& tree) {
    auto stream = DirStream::adopt(std::move(fd));
    if (!stream) {
        return std::unexpected(fail("open directory", display, stream.error()));
    }

    while (const dirent* entry = ::readdir(stream->get())) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        const auto st = stat_at(stream->fd(), name.c_str());
        if (!st) {
            if (st.error().value() == ENOENT) continue; // vanished mid-walk
            return std::unexpected(fail("stat", join(display, name), st.error()));
        }

        if (S_ISREG(st->st_mode)) {
            tree[join(rel, name)] = Info{static_cast<std::int64_t>(st->st_size),
                                         static_cast<std::int64_t>(st->st_mtim.tv_sec)};
        } else if (S_ISDIR(st->st_mode)) {
            auto sub = open_dir_at(stream->fd(), name.c_str());
            if (!sub) {
                if (sub.error().value() == ENOENT) continue;
                return std::unexpected(
                    fail("open directory", join(display, name), sub.error()));
            }
            if (auto walked =
                    walk(std::move(*sub), join(display, name), join(rel, name), tree);
                !walked) {
                return walked;
            }
        }
        // Symlinks, sockets, pipes, devices: not part of the v0 snapshot.
    }
    return {};
}

[[nodiscard]] std::expected<Tree, Failure> scan(const std::string& dir) {
    auto fd = open_dir_at(AT_FDCWD, dir.c_str());
    if (!fd) {
        return std::unexpected(fail("open directory", dir, fd.error()));
    }
    Tree tree;
    if (auto walked = walk(std::move(*fd), dir, "", tree); !walked) {
        return std::unexpected(walked.error());
    }
    return tree;
}

// --- Snapshot file I/O -------------------------------------------------------

[[nodiscard]] std::expected<std::string, std::error_code> read_file(
    const std::string& path) {
    const int raw = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (raw < 0) {
        return std::unexpected(std::error_code{errno, std::system_category()});
    }
    const Fd fd{raw};
    std::string content;
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd.get(), buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(std::error_code{errno, std::system_category()});
        }
        if (n == 0) break;
        content.append(buf, static_cast<std::size_t>(n));
    }
    return content;
}

// Parse "path size mtime" lines. The path may contain spaces: size and mtime
// are the last two space-separated fields.
[[nodiscard]] std::expected<Tree, Failure> parse_snapshot(const std::string& text) {
    Tree tree;
    std::size_t pos = 0;
    int lineno = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineno;
        if (line.empty()) continue;

        const auto malformed = [lineno] {
            return Failure{std::format("malformed snapshot line {}", lineno)};
        };

        const std::size_t sp2 = line.rfind(' ');
        if (sp2 == std::string::npos || sp2 == 0) return std::unexpected(malformed());
        const std::size_t sp1 = line.rfind(' ', sp2 - 1);
        if (sp1 == std::string::npos || sp1 == 0) return std::unexpected(malformed());

        const std::string path = line.substr(0, sp1);
        const std::string size_s = line.substr(sp1 + 1, sp2 - sp1 - 1);
        const std::string mtime_s = line.substr(sp2 + 1);

        Info info{};
        const auto [p1, e1] =
            std::from_chars(size_s.data(), size_s.data() + size_s.size(), info.size);
        const auto [p2, e2] = std::from_chars(mtime_s.data(),
                                              mtime_s.data() + mtime_s.size(), info.mtime);
        if (e1 != std::errc{} || p1 != size_s.data() + size_s.size() ||
            e2 != std::errc{} || p2 != mtime_s.data() + mtime_s.size()) {
            return std::unexpected(malformed());
        }
        tree[path] = info;
    }
    return tree;
}

// --- Subcommands -------------------------------------------------------------

int cmd_snapshot(const std::string& dir) {
    const auto tree = scan(dir);
    if (!tree) {
        std::println(stderr, "fwatch: error: {}", tree.error().message);
        return 1;
    }
    for (const auto& [path, info] : *tree) {
        std::println("{} {} {}", path, info.size, info.mtime);
    }
    return 0;
}

int cmd_diff(const std::string& dir, const std::string& snapshot_path) {
    const auto text = read_file(snapshot_path);
    if (!text) {
        std::println(stderr, "fwatch: error: cannot read snapshot '{}' (errno {})",
                     snapshot_path, text.error().value());
        return 1;
    }
    const auto before = parse_snapshot(*text);
    if (!before) {
        std::println(stderr, "fwatch: error: {}", before.error().message);
        return 1;
    }
    const auto after = scan(dir);
    if (!after) {
        std::println(stderr, "fwatch: error: {}", after.error().message);
        return 1;
    }

    // Sorted union of both key sets; each map is already ordered.
    std::vector<std::string> paths;
    paths.reserve(before->size() + after->size());
    for (const auto& [path, info] : *before) paths.push_back(path);
    for (const auto& [path, info] : *after) paths.push_back(path);
    std::ranges::sort(paths);
    const auto dead = std::ranges::unique(paths);
    paths.erase(dead.begin(), dead.end());

    int added = 0;
    int removed = 0;
    int modified = 0;
    for (const auto& path : paths) {
        const auto old_it = before->find(path);
        const auto new_it = after->find(path);
        if (old_it == before->end()) {
            std::println("+ {}", path);
            ++added;
        } else if (new_it == after->end()) {
            std::println("- {}", path);
            ++removed;
        } else if (old_it->second != new_it->second) {
            std::println("~ {}", path);
            ++modified;
        }
    }
    std::println("fwatch: {} added, {} removed, {} modified", added, removed, modified);
    return 0;
}

int usage() {
    std::println(stderr, "usage: fwatch snapshot DIR");
    std::println(stderr, "       fwatch diff DIR SNAPSHOT_FILE");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 2 && args[0] == "snapshot") {
        return cmd_snapshot(args[1]);
    }
    if (args.size() == 3 && args[0] == "diff") {
        return cmd_diff(args[1], args[2]);
    }
    return usage();
}
