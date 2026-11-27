#ifndef EXSIM_SERVER_JOURNAL_CANONICAL_HPP
#define EXSIM_SERVER_JOURNAL_CANONICAL_HPP
#include <server/journal/fields.hpp>
#include <array>
#include <bit>
#include <span>
namespace exsim::server::journal {
// Schema 3: the header records max_level_qty after max_orders. Each record holds
// a command (tag, then active fields, each an unsigned 64-bit big-endian slot;
// signed prices use their two's-complement bit pattern) and the output_digest of
// the events it produced.
inline constexpr std::uint64_t kSchema = 3;
inline void store64(unsigned char* p, std::uint64_t v) noexcept {
    for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(v >> (56 - 8 * i));
}
inline std::uint64_t load64(const unsigned char* p) noexcept {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
inline std::uint64_t hash_bytes(std::span<const unsigned char> bytes,
                                std::uint64_t h = 14695981039346656037ull) noexcept {
    for (auto b : bytes) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}
// Tag, then every active field, zero padded to the largest event.
template <class T>
std::array<unsigned char, 264> encode(const T& value) noexcept {
    std::array<unsigned char, 264> bytes{};
    store64(bytes.data(), static_cast<std::uint64_t>(value.type));
    std::size_t offset = 8;
    visit_fields(value, [&](const Field& f) {
        store64(bytes.data() + offset, f.raw);
        offset += 8;
    });
    return bytes;
}
// Digest of the events one command produced: the event count, then each event's type and
// fields, folded one 64-bit word at a time with the xxHash64 round (hand-rolled: one line,
// no dependency). The reactor pays two multiplies per field where hash_bytes would pay
// eight. Each round is a bijection of the running value and of the word, so one differing
// field always changes the digest; the rotation stops top-bit changes from cancelling in
// pairs, as they did with word-wise FNV-1a. The count comes first because zero words leave
// a zero running value at zero, which hid an extra all-zero Accepted event (tag 0).
inline std::uint64_t output_digest(std::span<const Event> events) noexcept {
    std::uint64_t h = 0;
    auto mix = [&h](std::uint64_t word) {
        h = std::rotl(h + word * 0xc2b2ae3d27d4eb4full, 31) * 0x9e3779b185ebca87ull;
    };
    mix(events.size());
    for (const Event& event : events) {
        mix(static_cast<std::uint64_t>(event.type));
        visit_fields(event, [&](const Field& f) { mix(f.raw); });
    }
    return h;
}
}  // namespace exsim::server::journal
#endif
