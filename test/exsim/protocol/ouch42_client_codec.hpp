#ifndef TEST_EXSIM_PROTOCOL_OUCH42_CLIENT_CODEC_HPP
#define TEST_EXSIM_PROTOCOL_OUCH42_CLIENT_CODEC_HPP
// OUCH 4.2 codec functions only a test client needs: request encoders, reply
// decoders, the replies the exchange never sends, and the span overloads.
#include <protocol/ouch42.hpp>
namespace exsim::protocol::ouch42 {
inline std::size_t encode(const ReplaceOrder& m, char* p, std::size_t n) noexcept {
    if (n < 47 || m.price < 0 || static_cast<std::uint64_t>(m.price) > UINT32_MAX) return 0;
    p[0] = 'U';
    std::copy_n(m.existing_token.data(), 14, p + 1);
    std::copy_n(m.replacement_token.data(), 14, p + 15);
    wire::store<4>(p + 29, static_cast<std::uint64_t>(m.shares));
    wire::store<4>(p + 33, static_cast<std::uint64_t>(m.price));
    wire::store<4>(p + 37, static_cast<std::uint64_t>(m.time_in_force));
    p[41] = m.display;
    p[42] = m.ise;
    wire::store<4>(p + 43, static_cast<std::uint64_t>(m.minimum_quantity));
    return 47;
}
inline std::size_t encode(const CancelOrder& m, char* p, std::size_t n) noexcept {
    if (n < 19) return 0;
    p[0] = 'X';
    std::copy_n(m.token.data(), 14, p + 1);
    wire::store<4>(p + 15, static_cast<std::uint64_t>(m.shares));
    return 19;
}
inline std::size_t encode(const ModifyOrder& m, char* p, std::size_t n) noexcept {
    if (n < 20) return 0;
    p[0] = 'M';
    std::copy_n(m.token.data(), 14, p + 1);
    p[15] = m.side;
    wire::store<4>(p + 16, static_cast<std::uint64_t>(m.shares));
    return 20;
}
inline Status decode(const char* p, std::size_t n, SystemEvent& m) noexcept {
    if (n < 10) return Status::incomplete;
    if (n != 10) return Status::invalid_length;
    if (p[0] != 'S') return Status::invalid_type;
    m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
    if (std::string_view("SE").find(p[9]) == std::string_view::npos) return Status::invalid_field;
    m.event = p[9];
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, OrderCanceled& m) noexcept {
    if (n < 28) return Status::incomplete;
    if (n != 28) return Status::invalid_length;
    if (p[0] != 'C') return Status::invalid_type;
    m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
    std::copy_n(p + 9, 14, m.token.data());
    m.shares = static_cast<Quantity>(wire::load<4>(p + 23));
    m.reason = p[27];
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, OrderExecuted& m) noexcept {
    if (n < 40) return Status::incomplete;
    if (n != 40) return Status::invalid_length;
    if (p[0] != 'E') return Status::invalid_type;
    m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
    std::copy_n(p + 9, 14, m.token.data());
    m.shares = static_cast<Quantity>(wire::load<4>(p + 23));
    m.price = static_cast<Price>(wire::load<4>(p + 27));
    m.liquidity = p[31];
    m.match_number = static_cast<std::uint64_t>(wire::load<8>(p + 32));
    return Status::ok;
}
struct CancelPending {
    static constexpr char type = 'P';
    static constexpr std::size_t wire_size = 23;
    std::uint64_t timestamp{};
    std::array<char, 14> token = wire::spaces<14>();
};
inline std::size_t encode(const CancelPending& m, char* p, std::size_t n) noexcept {
    if (n < 23) return 0;
    p[0] = 'P';
    wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
    std::copy_n(m.token.data(), 14, p + 9);
    return 23;
}
inline Status decode(const char* p, std::size_t n, CancelPending& m) noexcept {
    if (n < 23) return Status::incomplete;
    if (n != 23) return Status::invalid_length;
    if (p[0] != 'P') return Status::invalid_type;
    m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
    std::copy_n(p + 9, 14, m.token.data());
    return Status::ok;
}
struct CancelReject {
    static constexpr char type = 'I';
    static constexpr std::size_t wire_size = 23;
    std::uint64_t timestamp{};
    std::array<char, 14> token = wire::spaces<14>();
};
inline std::size_t encode(const CancelReject& m, char* p, std::size_t n) noexcept {
    if (n < 23) return 0;
    p[0] = 'I';
    wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
    std::copy_n(m.token.data(), 14, p + 9);
    return 23;
}
inline Status decode(const char* p, std::size_t n, CancelReject& m) noexcept {
    if (n < 23) return Status::incomplete;
    if (n != 23) return Status::invalid_length;
    if (p[0] != 'I') return Status::invalid_type;
    m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
    std::copy_n(p + 9, 14, m.token.data());
    return Status::ok;
}
struct OrderModified {
    static constexpr char type = 'M';
    static constexpr std::size_t wire_size = 28;
    std::uint64_t timestamp{};
    std::array<char, 14> token = wire::spaces<14>();
    char side = 'B';
    Quantity shares{};
};
inline std::size_t encode(const OrderModified& m, char* p, std::size_t n) noexcept {
    if (n < 28) return 0;
    p[0] = 'M';
    wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
    std::copy_n(m.token.data(), 14, p + 9);
    p[23] = m.side;
    wire::store<4>(p + 24, static_cast<std::uint64_t>(m.shares));
    return 28;
}
inline Status decode(const char* p, std::size_t n, OrderModified& m) noexcept {
    if (n < 28) return Status::incomplete;
    if (n != 28) return Status::invalid_length;
    if (p[0] != 'M') return Status::invalid_type;
    m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
    std::copy_n(p + 9, 14, m.token.data());
    if (std::string_view("BSTE").find(p[23]) == std::string_view::npos) return Status::invalid_field;
    m.side = p[23];
    m.shares = static_cast<Quantity>(wire::load<4>(p + 24));
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, OrderReplaced& m) noexcept {
    if (n < 80) return Status::incomplete;
    if (n != 80) return Status::invalid_length;
    if (p[0] != 'U') return Status::invalid_type;
    m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
    std::copy_n(p + 9, 14, m.replacement_token.data());
    if (std::string_view("BSTE").find(p[23]) == std::string_view::npos) return Status::invalid_field;
    m.side = p[23];
    m.shares = static_cast<Quantity>(wire::load<4>(p + 24));
    std::copy_n(p + 28, 8, m.stock.chars.data());
    m.price = static_cast<Price>(wire::load<4>(p + 36));
    m.time_in_force = static_cast<std::uint32_t>(wire::load<4>(p + 40));
    std::copy_n(p + 44, 4, m.firm.data());
    m.display = p[48];
    m.order_reference = static_cast<std::uint64_t>(wire::load<8>(p + 49));
    m.capacity = p[57];
    m.ise = p[58];
    m.minimum_quantity = static_cast<Quantity>(wire::load<4>(p + 59));
    m.cross_type = p[63];
    if (std::string_view("LD").find(p[64]) == std::string_view::npos) return Status::invalid_field;
    m.order_state = p[64];
    std::copy_n(p + 65, 14, m.previous_token.data());
    m.bbo_weight = p[79];
    return Status::ok;
}
inline Status decode(std::span<const char> p, EnterOrder& m) noexcept { return decode(p.data(), p.size(), m); }
inline Status decode(std::span<const char> p, ReplaceOrder& m) noexcept { return decode(p.data(), p.size(), m); }
inline Status decode(std::span<const char> p, CancelOrder& m) noexcept { return decode(p.data(), p.size(), m); }
inline Status decode(std::span<const char> p, ModifyOrder& m) noexcept { return decode(p.data(), p.size(), m); }
inline Status decode(std::span<const char> p, SystemEvent& m) noexcept { return decode(p.data(), p.size(), m); }
inline Status decode(std::span<const char> p, OrderAccepted& m) noexcept { return decode(p.data(), p.size(), m); }
inline Status decode(std::span<const char> p, OrderCanceled& m) noexcept { return decode(p.data(), p.size(), m); }
inline Status decode(std::span<const char> p, OrderExecuted& m) noexcept { return decode(p.data(), p.size(), m); }
inline Status decode(std::span<const char> p, OrderRejected& m) noexcept { return decode(p.data(), p.size(), m); }
inline Status decode(std::span<const char> p, CancelPending& m) noexcept { return decode(p.data(), p.size(), m); }
inline Status decode(std::span<const char> p, CancelReject& m) noexcept { return decode(p.data(), p.size(), m); }
inline Status decode(std::span<const char> p, OrderModified& m) noexcept { return decode(p.data(), p.size(), m); }
inline Status decode(std::span<const char> p, OrderReplaced& m) noexcept { return decode(p.data(), p.size(), m); }
}  // namespace exsim::protocol::ouch42
#endif
