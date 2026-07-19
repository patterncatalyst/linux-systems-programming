// proto.hpp — the canonical chatterd chat frame (ch21, unchanged since):
// magic "CH", version 1, one byte type, big-endian u16 length, then payload.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <system_error>
#include <vector>

namespace proto {

constexpr std::uint8_t kMagic0 = 'C';
constexpr std::uint8_t kMagic1 = 'H';
constexpr std::uint8_t kVersion = 1;

constexpr std::uint8_t kJoin = 1;
constexpr std::uint8_t kMsg = 2;
constexpr std::uint8_t kDeliver = 3;

struct Frame {
    std::uint8_t type{};
    std::vector<std::uint8_t> payload;
};

// Writes the 6-byte header + payload to fd. Returns an error_code on a
// short/failed write.
[[nodiscard]] std::expected<void, std::error_code> write_frame(int fd, const Frame& f);

// Reads one frame from fd (blocking on a normal, non-nonblocking socket).
// Returns an error_code on EOF/short-read/bad magic.
[[nodiscard]] std::expected<Frame, std::error_code> read_frame(int fd);

// "nick NUL text" DELIVER payload (ch21).
std::vector<std::uint8_t> deliver_payload(const std::string& nick, const std::string& text);

// Splits "nick NUL text"; returns false if there's no NUL.
bool split_deliver(const std::vector<std::uint8_t>& payload, std::string& nick, std::string& text);

} // namespace proto
