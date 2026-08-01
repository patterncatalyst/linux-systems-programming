#include "digest.hpp"

namespace {
constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;
} // namespace

Digest fnv1a(const std::uint8_t* data, std::size_t len) {
    std::uint64_t h = kFnvOffsetBasis;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint64_t>(data[i]);
        h *= kFnvPrime;
    }
    return Digest{h};
}
