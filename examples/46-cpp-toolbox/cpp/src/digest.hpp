#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// A custom aggregate wrapping a single 64-bit hash value. Small on purpose:
// it exists so the gdb chapter (`.gdbinit` + `toolbox-printers.py`) has a
// concrete, non-trivial type to pretty-print instead of a bare integer.
struct Digest {
    std::uint64_t fnv;
};

// FNV-1a over a fixed, embedded byte array. Integer/string-only, no floats,
// no addresses, no timing, no unordered-container iteration -- the same
// input bytes must produce the same digest bit-for-bit on every conforming
// C++23 compiler, which is what the GCC-vs-clang parity gate (ch46 Sec.
// "GCC-vs-clang parity") relies on.
Digest fnv1a(const std::uint8_t* data, std::size_t len);

// The fixed payload `report` hashes. Extracted so `toolbox.cpp` and any
// future caller share one definition.
inline constexpr std::array<std::uint8_t, 16> kPayload = {
    0x54, 0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63, 0x6b, 0x20, 0x62, 0x72, 0x6f, 0x77, 0x6e, 0x2e,
};
