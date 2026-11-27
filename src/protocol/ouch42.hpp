#ifndef EXSIM_PROTOCOL_OUCH42_HPP
#define EXSIM_PROTOCOL_OUCH42_HPP
// Layout reference: Nasdaq OUCH 4.2 (October 2025), sections 2 and 3.
// https://nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH4.2.pdf
// Typed overloads disambiguate inbound and outbound U/M messages. Tokens are
// fixed alpha fields; use wire::alpha(token.data(), token.size(), text) to set
// them. Unknown extensible alpha codes are preserved; business validation
// belongs to the gateway. Prices use exsim::Price, checked against the 32-bit
// wire range. Client-side functions with no caller outside the tests live in
// test/exsim/protocol/ouch42_client_codec.hpp.
#include "engine/instrument.hpp"
#include "common/types.hpp"
#include "wire.hpp"
#include <algorithm>
namespace exsim::protocol::ouch42 {
using protocol::Status;
struct EnterOrder {
    static constexpr char type = 'O';
    static constexpr std::size_t wire_size = 49;
    std::array<char, 14> token = wire::spaces<14>();
    char side = 'B';
    Quantity shares{};
    Symbol stock{};
    Price price{};
    std::uint32_t time_in_force{};
    std::array<char, 4> firm = wire::spaces<4>();
    char display{};
    char capacity{};
    char ise{};
    Quantity minimum_quantity{};
    char cross_type{};
    char customer_type{};
};
inline std::size_t encode(const EnterOrder& m, char* p, std::size_t n) noexcept {
    if (n < 49 || m.price < 0 || static_cast<std::uint64_t>(m.price) > UINT32_MAX) return 0;
    p[0] = 'O';
    std::copy_n(m.token.data(), 14, p + 1);
    p[15] = m.side;
    wire::store<4>(p + 16, static_cast<std::uint64_t>(m.shares));
    std::copy_n(m.stock.chars.data(), 8, p + 20);
    wire::store<4>(p + 28, static_cast<std::uint64_t>(m.price));
    wire::store<4>(p + 32, static_cast<std::uint64_t>(m.time_in_force));
    std::copy_n(m.firm.data(), 4, p + 36);
    p[40] = m.display;
    p[41] = m.capacity;
    p[42] = m.ise;
    wire::store<4>(p + 43, static_cast<std::uint64_t>(m.minimum_quantity));
    p[47] = m.cross_type;
    p[48] = m.customer_type;
    return 49;
}
inline Status decode(const char* p, std::size_t n, EnterOrder& m) noexcept {
    if (n < 49) return Status::incomplete;
    if (n != 49) return Status::invalid_length;
    if (p[0] != 'O') return Status::invalid_type;
    std::copy_n(p + 1, 14, m.token.data());
    if (std::string_view("BSTE").find(p[15]) == std::string_view::npos) return Status::invalid_field;
    m.side = p[15];
    m.shares = static_cast<Quantity>(wire::load<4>(p + 16));
    std::copy_n(p + 20, 8, m.stock.chars.data());
    m.price = static_cast<Price>(wire::load<4>(p + 28));
    m.time_in_force = static_cast<std::uint32_t>(wire::load<4>(p + 32));
    std::copy_n(p + 36, 4, m.firm.data());
    m.display = p[40];
    m.capacity = p[41];
    m.ise = p[42];
    m.minimum_quantity = static_cast<Quantity>(wire::load<4>(p + 43));
    m.cross_type = p[47];
    m.customer_type = p[48];
    return Status::ok;
}
struct ReplaceOrder {
    static constexpr char type = 'U';
    static constexpr std::size_t wire_size = 47;
    std::array<char, 14> existing_token = wire::spaces<14>();
    std::array<char, 14> replacement_token = wire::spaces<14>();
    Quantity shares{};
    Price price{};
    std::uint32_t time_in_force{};
    char display{};
    char ise{};
    Quantity minimum_quantity{};
};
inline Status decode(const char* p, std::size_t n, ReplaceOrder& m) noexcept {
    if (n < 47) return Status::incomplete;
    if (n != 47) return Status::invalid_length;
    if (p[0] != 'U') return Status::invalid_type;
    std::copy_n(p + 1, 14, m.existing_token.data());
    std::copy_n(p + 15, 14, m.replacement_token.data());
    m.shares = static_cast<Quantity>(wire::load<4>(p + 29));
    m.price = static_cast<Price>(wire::load<4>(p + 33));
    m.time_in_force = static_cast<std::uint32_t>(wire::load<4>(p + 37));
    m.display = p[41];
    m.ise = p[42];
    m.minimum_quantity = static_cast<Quantity>(wire::load<4>(p + 43));
    return Status::ok;
}
struct CancelOrder {
    static constexpr char type = 'X';
    static constexpr std::size_t wire_size = 19;
    std::array<char, 14> token = wire::spaces<14>();
    Quantity shares{};
};
inline Status decode(const char* p, std::size_t n, CancelOrder& m) noexcept {
    if (n < 19) return Status::incomplete;
    if (n != 19) return Status::invalid_length;
    if (p[0] != 'X') return Status::invalid_type;
    std::copy_n(p + 1, 14, m.token.data());
    m.shares = static_cast<Quantity>(wire::load<4>(p + 15));
    return Status::ok;
}
struct ModifyOrder {
    static constexpr char type = 'M';
    static constexpr std::size_t wire_size = 20;
    std::array<char, 14> token = wire::spaces<14>();
    char side = 'B';
    Quantity shares{};
};
inline Status decode(const char* p, std::size_t n, ModifyOrder& m) noexcept {
    if (n < 20) return Status::incomplete;
    if (n != 20) return Status::invalid_length;
    if (p[0] != 'M') return Status::invalid_type;
    std::copy_n(p + 1, 14, m.token.data());
    if (std::string_view("BSTE").find(p[15]) == std::string_view::npos) return Status::invalid_field;
    m.side = p[15];
    m.shares = static_cast<Quantity>(wire::load<4>(p + 16));
    return Status::ok;
}
struct SystemEvent {
    static constexpr char type = 'S';
    static constexpr std::size_t wire_size = 10;
    std::uint64_t timestamp{};
    char event = 'S';
};
inline std::size_t encode(const SystemEvent& m, char* p, std::size_t n) noexcept {
    if (n < 10) return 0;
    p[0] = 'S';
    wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
    p[9] = m.event;
    return 10;
}
struct OrderAccepted {
    static constexpr char type = 'A';
    static constexpr std::size_t wire_size = 66;
    std::uint64_t timestamp{};
    std::array<char, 14> token = wire::spaces<14>();
    char side = 'B';
    Quantity shares{};
    Symbol stock{};
    Price price{};
    std::uint32_t time_in_force{};
    std::array<char, 4> firm = wire::spaces<4>();
    char display{};
    std::uint64_t order_reference{};
    char capacity{};
    char ise{};
    Quantity minimum_quantity{};
    char cross_type{};
    char order_state = 'L';
    char bbo_weight{};
};
inline std::size_t encode(const OrderAccepted& m, char* p, std::size_t n) noexcept {
    if (n < 66 || m.price < 0 || static_cast<std::uint64_t>(m.price) > UINT32_MAX) return 0;
    p[0] = 'A';
    wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
    std::copy_n(m.token.data(), 14, p + 9);
    p[23] = m.side;
    wire::store<4>(p + 24, static_cast<std::uint64_t>(m.shares));
    std::copy_n(m.stock.chars.data(), 8, p + 28);
    wire::store<4>(p + 36, static_cast<std::uint64_t>(m.price));
    wire::store<4>(p + 40, static_cast<std::uint64_t>(m.time_in_force));
    std::copy_n(m.firm.data(), 4, p + 44);
    p[48] = m.display;
    wire::store<8>(p + 49, static_cast<std::uint64_t>(m.order_reference));
    p[57] = m.capacity;
    p[58] = m.ise;
    wire::store<4>(p + 59, static_cast<std::uint64_t>(m.minimum_quantity));
    p[63] = m.cross_type;
    p[64] = m.order_state;
    p[65] = m.bbo_weight;
    return 66;
}
inline Status decode(const char* p, std::size_t n, OrderAccepted& m) noexcept {
    if (n < 66) return Status::incomplete;
    if (n != 66) return Status::invalid_length;
    if (p[0] != 'A') return Status::invalid_type;
    m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
    std::copy_n(p + 9, 14, m.token.data());
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
    m.bbo_weight = p[65];
    return Status::ok;
}
struct OrderCanceled {
    static constexpr char type = 'C';
    static constexpr std::size_t wire_size = 28;
    std::uint64_t timestamp{};
    std::array<char, 14> token = wire::spaces<14>();
    Quantity shares{};
    char reason{};
};
inline std::size_t encode(const OrderCanceled& m, char* p, std::size_t n) noexcept {
    if (n < 28) return 0;
    p[0] = 'C';
    wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
    std::copy_n(m.token.data(), 14, p + 9);
    wire::store<4>(p + 23, static_cast<std::uint64_t>(m.shares));
    p[27] = m.reason;
    return 28;
}
struct OrderExecuted {
    static constexpr char type = 'E';
    static constexpr std::size_t wire_size = 40;
    std::uint64_t timestamp{};
    std::array<char, 14> token = wire::spaces<14>();
    Quantity shares{};
    Price price{};
    char liquidity{};
    std::uint64_t match_number{};
};
inline std::size_t encode(const OrderExecuted& m, char* p, std::size_t n) noexcept {
    if (n < 40 || m.price < 0 || static_cast<std::uint64_t>(m.price) > UINT32_MAX) return 0;
    p[0] = 'E';
    wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
    std::copy_n(m.token.data(), 14, p + 9);
    wire::store<4>(p + 23, static_cast<std::uint64_t>(m.shares));
    wire::store<4>(p + 27, static_cast<std::uint64_t>(m.price));
    p[31] = m.liquidity;
    wire::store<8>(p + 32, static_cast<std::uint64_t>(m.match_number));
    return 40;
}
struct OrderRejected {
    static constexpr char type = 'J';
    static constexpr std::size_t wire_size = 24;
    std::uint64_t timestamp{};
    std::array<char, 14> token = wire::spaces<14>();
    char reason{};
};
inline std::size_t encode(const OrderRejected& m, char* p, std::size_t n) noexcept {
    if (n < 24) return 0;
    p[0] = 'J';
    wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
    std::copy_n(m.token.data(), 14, p + 9);
    p[23] = m.reason;
    return 24;
}
inline Status decode(const char* p, std::size_t n, OrderRejected& m) noexcept {
    if (n < 24) return Status::incomplete;
    if (n != 24) return Status::invalid_length;
    if (p[0] != 'J') return Status::invalid_type;
    m.timestamp = static_cast<std::uint64_t>(wire::load<8>(p + 1));
    std::copy_n(p + 9, 14, m.token.data());
    m.reason = p[23];
    return Status::ok;
}
struct OrderReplaced {
    static constexpr char type = 'U';
    static constexpr std::size_t wire_size = 80;
    std::uint64_t timestamp{};
    std::array<char, 14> replacement_token = wire::spaces<14>();
    char side = 'B';
    Quantity shares{};
    Symbol stock{};
    Price price{};
    std::uint32_t time_in_force{};
    std::array<char, 4> firm = wire::spaces<4>();
    char display{};
    std::uint64_t order_reference{};
    char capacity{};
    char ise{};
    Quantity minimum_quantity{};
    char cross_type{};
    char order_state = 'L';
    std::array<char, 14> previous_token = wire::spaces<14>();
    char bbo_weight{};
};
inline std::size_t encode(const OrderReplaced& m, char* p, std::size_t n) noexcept {
    if (n < 80 || m.price < 0 || static_cast<std::uint64_t>(m.price) > UINT32_MAX) return 0;
    p[0] = 'U';
    wire::store<8>(p + 1, static_cast<std::uint64_t>(m.timestamp));
    std::copy_n(m.replacement_token.data(), 14, p + 9);
    p[23] = m.side;
    wire::store<4>(p + 24, static_cast<std::uint64_t>(m.shares));
    std::copy_n(m.stock.chars.data(), 8, p + 28);
    wire::store<4>(p + 36, static_cast<std::uint64_t>(m.price));
    wire::store<4>(p + 40, static_cast<std::uint64_t>(m.time_in_force));
    std::copy_n(m.firm.data(), 4, p + 44);
    p[48] = m.display;
    wire::store<8>(p + 49, static_cast<std::uint64_t>(m.order_reference));
    p[57] = m.capacity;
    p[58] = m.ise;
    wire::store<4>(p + 59, static_cast<std::uint64_t>(m.minimum_quantity));
    p[63] = m.cross_type;
    p[64] = m.order_state;
    std::copy_n(m.previous_token.data(), 14, p + 65);
    p[79] = m.bbo_weight;
    return 80;
}
}  // namespace exsim::protocol::ouch42
#endif
