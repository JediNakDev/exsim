#ifndef EXSIM_SERVER_JOURNAL_CANONICAL_HPP
#define EXSIM_SERVER_JOURNAL_CANONICAL_HPP
#include <server/journal/fields.hpp>
#include <span>
namespace exsim::server::journal {
// Schema 2: tag followed by active fields, each an unsigned 64-bit big-endian
// slot; signed prices use their two's-complement bit pattern.
inline constexpr std::uint64_t kSchema = 2;
inline void store64(unsigned char* p, std::uint64_t v) noexcept {
    for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(v >> (56 - 8 * i));
}
inline std::uint64_t hash_bytes(std::span<const unsigned char> bytes,
                                std::uint64_t h = 14695981039346656037ull) noexcept {
    for (auto b : bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}
}  // namespace exsim::server::journal
#endif
