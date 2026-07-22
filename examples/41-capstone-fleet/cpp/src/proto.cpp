#include "proto.hpp"

#include <cerrno>
#include <unistd.h>

namespace proto {

namespace {

[[nodiscard]] std::expected<void, std::error_code> write_all(int fd, const std::uint8_t* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(std::error_code{errno, std::system_category()});
        }
        if (n == 0) return std::unexpected(std::make_error_code(std::errc::io_error));
        off += static_cast<std::size_t>(n);
    }
    return {};
}

[[nodiscard]] std::expected<void, std::error_code> read_all(int fd, std::uint8_t* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        ssize_t n = ::read(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(std::error_code{errno, std::system_category()});
        }
        if (n == 0) return std::unexpected(std::make_error_code(std::errc::connection_reset));
        off += static_cast<std::size_t>(n);
    }
    return {};
}

} // namespace

std::expected<void, std::error_code> write_frame(int fd, const Frame& f) {
    if (f.payload.size() > 0xFFFF) {
        return std::unexpected(std::make_error_code(std::errc::message_size));
    }
    std::uint8_t hdr[6];
    hdr[0] = kMagic0;
    hdr[1] = kMagic1;
    hdr[2] = kVersion;
    hdr[3] = f.type;
    hdr[4] = static_cast<std::uint8_t>((f.payload.size() >> 8) & 0xFF);
    hdr[5] = static_cast<std::uint8_t>(f.payload.size() & 0xFF);
    if (auto r = write_all(fd, hdr, sizeof(hdr)); !r) return r;
    if (!f.payload.empty()) {
        if (auto r = write_all(fd, f.payload.data(), f.payload.size()); !r) return r;
    }
    return {};
}

std::expected<Frame, std::error_code> read_frame(int fd) {
    std::uint8_t hdr[6];
    if (auto r = read_all(fd, hdr, sizeof(hdr)); !r) return std::unexpected(r.error());
    if (hdr[0] != kMagic0 || hdr[1] != kMagic1 || hdr[2] != kVersion) {
        return std::unexpected(std::make_error_code(std::errc::protocol_error));
    }
    Frame f;
    f.type = hdr[3];
    std::size_t len = (static_cast<std::size_t>(hdr[4]) << 8) | hdr[5];
    f.payload.resize(len);
    if (len > 0) {
        if (auto r = read_all(fd, f.payload.data(), len); !r) return std::unexpected(r.error());
    }
    return f;
}

std::vector<std::uint8_t> deliver_payload(const std::string& nick, const std::string& text) {
    std::vector<std::uint8_t> out;
    out.reserve(nick.size() + 1 + text.size());
    out.insert(out.end(), nick.begin(), nick.end());
    out.push_back(0);
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

bool split_deliver(const std::vector<std::uint8_t>& payload, std::string& nick, std::string& text) {
    for (std::size_t i = 0; i < payload.size(); ++i) {
        if (payload[i] == 0) {
            nick.assign(payload.begin(), payload.begin() + static_cast<long>(i));
            text.assign(payload.begin() + static_cast<long>(i) + 1, payload.end());
            return true;
        }
    }
    return false;
}

} // namespace proto
