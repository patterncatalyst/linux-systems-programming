// shmkv v0 — a tiny mmap-backed key/value store (C++23).
//
// On-disk format, byte-exact and shared with the Go and Rust implementations:
//
//   offset 0   6 bytes   magic "SHKV1\0"            (53 48 4b 56 31 00)
//   offset 6   4 bytes   u32 little-endian slot_count
//   offset 10  slot_count x 256-byte slots:
//              [  0.. 64)  key,   NUL-padded (max 63 bytes, key[0]==0 => empty)
//              [ 64..256)  value, NUL-padded (max 191 bytes)
//
// Every process maps the file MAP_SHARED, so a `set` from one binary is
// immediately visible to a `get` from another; msync(MS_SYNC) pushes the
// dirty pages to disk before we report success.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr std::array<unsigned char, 6> kMagic{'S', 'H', 'K', 'V', '1', '\0'};
constexpr std::size_t kHeaderSize = 10;
constexpr std::size_t kKeyField = 64;
constexpr std::size_t kValueField = 192;
constexpr std::size_t kSlotSize = kKeyField + kValueField;
constexpr std::size_t kKeyMax = kKeyField - 1;     // 63: room for one NUL
constexpr std::size_t kValueMax = kValueField - 1; // 191

// A command failure: fixed message for stderr plus the process exit code.
struct CliError {
    int code;
    std::string msg;
};

template <typename T = void>
using Result = std::expected<T, CliError>;

[[nodiscard]] CliError fail(int code, std::string msg) { return {code, std::move(msg)}; }

// RAII file descriptor.
class Fd {
  public:
    explicit Fd(int fd) noexcept : fd_(fd) {}
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
    ~Fd() { reset(); }
    [[nodiscard]] int get() const noexcept { return fd_; }

  private:
    void reset() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    int fd_ = -1;
};

// RAII shared mapping: mmap on construction, munmap in the destructor,
// msync(MS_SYNC) on demand. MAP_SHARED means stores are visible to every
// other process mapping the same file, and reach the page cache (and disk,
// after msync) rather than staying private to this address space.
class Mapping {
  public:
    static Result<Mapping> map_shared(const Fd& fd, std::size_t len) {
        void* p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
        if (p == MAP_FAILED) {
            return std::unexpected(fail(1, "shmkv: mmap failed"));
        }
        return Mapping{static_cast<unsigned char*>(p), len};
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Mapping(Mapping&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), len_(std::exchange(other.len_, 0)) {}
    Mapping& operator=(Mapping&& other) noexcept {
        if (this != &other) {
            reset();
            data_ = std::exchange(other.data_, nullptr);
            len_ = std::exchange(other.len_, 0);
        }
        return *this;
    }
    ~Mapping() { reset(); }

    [[nodiscard]] unsigned char* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return len_; }

    [[nodiscard]] Result<> sync() const {
        if (::msync(data_, len_, MS_SYNC) != 0) {
            return std::unexpected(fail(1, "shmkv: msync failed"));
        }
        return {};
    }

  private:
    Mapping(unsigned char* data, std::size_t len) noexcept : data_(data), len_(len) {}
    void reset() noexcept {
        if (data_ != nullptr) ::munmap(data_, len_);
        data_ = nullptr;
        len_ = 0;
    }
    unsigned char* data_ = nullptr;
    std::size_t len_ = 0;
};

// An open store: owns the fd and the mapping, decodes the header once.
class Store {
  public:
    static Result<Store> create(const std::string& path, std::uint32_t slots) {
        // O_TRUNC first so a reused path starts from zero bytes; the
        // following ftruncate then extends with guaranteed-zero pages,
        // which is what makes every slot "empty" (key[0] == 0) for free.
        Fd fd{::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644)};
        if (fd.get() < 0) {
            return std::unexpected(fail(1, "shmkv: cannot open " + path));
        }
        const std::size_t len = kHeaderSize + static_cast<std::size_t>(slots) * kSlotSize;
        if (::ftruncate(fd.get(), static_cast<off_t>(len)) != 0) {
            return std::unexpected(fail(1, "shmkv: ftruncate failed on " + path));
        }
        auto map = Mapping::map_shared(fd, len);
        if (!map) return std::unexpected(map.error());
        std::memcpy(map->data(), kMagic.data(), kMagic.size());
        const std::array<unsigned char, 4> le{
            static_cast<unsigned char>(slots & 0xff),
            static_cast<unsigned char>((slots >> 8) & 0xff),
            static_cast<unsigned char>((slots >> 16) & 0xff),
            static_cast<unsigned char>((slots >> 24) & 0xff),
        };
        std::memcpy(map->data() + kMagic.size(), le.data(), le.size());
        if (auto s = map->sync(); !s) return std::unexpected(s.error());
        return Store{std::move(fd), std::move(*map), slots};
    }

    static Result<Store> open(const std::string& path) {
        Fd fd{::open(path.c_str(), O_RDWR)};
        if (fd.get() < 0) {
            return std::unexpected(fail(1, "shmkv: cannot open " + path));
        }
        struct stat st{};
        if (::fstat(fd.get(), &st) != 0) {
            return std::unexpected(fail(1, "shmkv: cannot open " + path));
        }
        const auto len = static_cast<std::size_t>(st.st_size);
        CliError bad = fail(1, "shmkv: " + path + ": not a shmkv v0 store");
        if (len < kHeaderSize) return std::unexpected(std::move(bad));
        auto map = Mapping::map_shared(fd, len);
        if (!map) return std::unexpected(map.error());
        if (std::memcmp(map->data(), kMagic.data(), kMagic.size()) != 0) {
            return std::unexpected(std::move(bad));
        }
        std::uint32_t slots = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            slots |= static_cast<std::uint32_t>(map->data()[kMagic.size() + i]) << (8 * i);
        }
        if (len != kHeaderSize + static_cast<std::size_t>(slots) * kSlotSize) {
            return std::unexpected(std::move(bad));
        }
        return Store{std::move(fd), std::move(*map), slots};
    }

    [[nodiscard]] std::uint32_t slot_count() const noexcept { return slots_; }

    [[nodiscard]] std::string_view slot_key(std::uint32_t i) const {
        const unsigned char* s = map_.data() + kHeaderSize + std::size_t{i} * kSlotSize;
        return field(s, kKeyField);
    }

    [[nodiscard]] std::string_view slot_value(std::uint32_t i) const {
        const unsigned char* s = map_.data() + kHeaderSize + std::size_t{i} * kSlotSize;
        return field(s + kKeyField, kValueField);
    }

    // Linear probe: overwrite the slot holding `key`, else claim the first
    // empty slot. Exit-5 failure when every slot is taken.
    [[nodiscard]] Result<> set(std::string_view key, std::string_view value) {
        std::uint32_t target = slots_;
        bool overwrite = false;
        std::uint32_t first_empty = slots_;
        for (std::uint32_t i = 0; i < slots_; ++i) {
            const auto k = slot_key(i);
            if (k == key) {
                target = i;
                overwrite = true;
                break;
            }
            if (k.empty() && first_empty == slots_) first_empty = i;
        }
        if (!overwrite) target = first_empty;
        if (target == slots_) {
            return std::unexpected(fail(5, std::format("shmkv: store full ({} slots)", slots_)));
        }
        unsigned char* s = map_.data() + kHeaderSize + std::size_t{target} * kSlotSize;
        std::memset(s, 0, kSlotSize); // clear any longer previous value
        std::memcpy(s, key.data(), key.size());
        std::memcpy(s + kKeyField, value.data(), value.size());
        return map_.sync();
    }

  private:
    Store(Fd fd, Mapping map, std::uint32_t slots) noexcept
        : fd_(std::move(fd)), map_(std::move(map)), slots_(slots) {}

    [[nodiscard]] static std::string_view field(const unsigned char* p, std::size_t max) {
        const auto* nul = static_cast<const unsigned char*>(std::memchr(p, 0, max));
        const std::size_t n = (nul != nullptr) ? static_cast<std::size_t>(nul - p) : max;
        return {reinterpret_cast<const char*>(p), n};
    }

    Fd fd_;
    Mapping map_;
    std::uint32_t slots_;
};

[[nodiscard]] Result<std::uint32_t> parse_slots(std::string_view text) {
    // Digits only, in range [1, u32 max] — identical rules in all three
    // languages so the CLIs reject exactly the same inputs.
    if (text.empty() || text.size() > 10 ||
        !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::unexpected(fail(2, ""));
    }
    std::uint64_t v = 0;
    for (const char c : text) v = v * 10 + static_cast<std::uint64_t>(c - '0');
    if (v == 0 || v > 0xffffffffULL) return std::unexpected(fail(2, ""));
    return static_cast<std::uint32_t>(v);
}

[[nodiscard]] Result<> cmd_create(const std::string& file, std::string_view slots_text) {
    const auto slots = parse_slots(slots_text);
    if (!slots) return std::unexpected(slots.error());
    auto store = Store::create(file, *slots);
    if (!store) return std::unexpected(store.error());
    const std::size_t bytes = kHeaderSize + std::size_t{*slots} * kSlotSize;
    std::println("created {}: {} slots, {} bytes", file, *slots, bytes);
    return {};
}

[[nodiscard]] Result<> cmd_set(const std::string& file, std::string_view key,
                               std::string_view value) {
    if (key.empty()) return std::unexpected(fail(2, "shmkv: empty key"));
    if (key.size() > kKeyMax) {
        return std::unexpected(fail(2, "shmkv: key too long (max 63 bytes)"));
    }
    if (value.size() > kValueMax) {
        return std::unexpected(fail(2, "shmkv: value too long (max 191 bytes)"));
    }
    auto store = Store::open(file);
    if (!store) return std::unexpected(store.error());
    if (auto r = store->set(key, value); !r) return std::unexpected(r.error());
    std::println("set {}", key);
    return {};
}

[[nodiscard]] Result<> cmd_get(const std::string& file, std::string_view key) {
    auto store = Store::open(file);
    if (!store) return std::unexpected(store.error());
    for (std::uint32_t i = 0; i < store->slot_count(); ++i) {
        if (store->slot_key(i) == key) {
            std::println("{}", store->slot_value(i));
            return {};
        }
    }
    return std::unexpected(fail(4, "shmkv: key not found"));
}

[[nodiscard]] Result<> cmd_dump(const std::string& file) {
    auto store = Store::open(file);
    if (!store) return std::unexpected(store.error());
    std::vector<std::pair<std::string, std::string>> pairs;
    for (std::uint32_t i = 0; i < store->slot_count(); ++i) {
        const auto k = store->slot_key(i);
        if (!k.empty()) pairs.emplace_back(std::string{k}, std::string{store->slot_value(i)});
    }
    std::ranges::sort(pairs); // bytewise by key, matching Go/Rust
    for (const auto& [k, v] : pairs) std::println("{}={}", k, v);
    // Flush before the stderr summary: stdout is fully buffered on a pipe,
    // and Go/Rust emit the pairs first — keep the interleaving identical.
    std::fflush(stdout);
    std::println(stderr, "shmkv: {}/{} slots used", pairs.size(), store->slot_count());
    return {};
}

int usage() {
    std::println(stderr, "usage: shmkv create FILE --slots N | set FILE KEY VALUE | "
                         "get FILE KEY | dump FILE");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    Result<> r;
    if (args.size() == 4 && args[0] == "create" && args[2] == "--slots") {
        r = cmd_create(args[1], args[3]);
    } else if (args.size() == 4 && args[0] == "set") {
        r = cmd_set(args[1], args[2], args[3]);
    } else if (args.size() == 3 && args[0] == "get") {
        r = cmd_get(args[1], args[2]);
    } else if (args.size() == 2 && args[0] == "dump") {
        r = cmd_dump(args[1]);
    } else {
        return usage();
    }
    if (!r) {
        if (r.error().msg.empty()) return usage();
        std::println(stderr, "{}", r.error().msg);
        return r.error().code;
    }
    return 0;
}
