#ifndef TEST_EXSIM_PROTOCOL_FIX44_CLIENT_CODEC_HPP
#define TEST_EXSIM_PROTOCOL_FIX44_CLIENT_CODEC_HPP
// Typed FIX 4.4 client requests and SequenceReset, with their encoders; only tests
// use them. The exchange reads requests through Parser and FixFieldReader and
// builds its GapFill with Builder.
#include <protocol/fix44.hpp>
namespace exsim::protocol::fix44 {
constexpr char tif_char(TimeInForce t) noexcept {
    switch (t) {
        case TimeInForce::Day: return '0';
        case TimeInForce::GTC: return '1';
        case TimeInForce::IOC: return '3';
        case TimeInForce::FOK: return '4';
    }
    return '?';
}
constexpr char stp_char(StpMode t) noexcept {
    switch (t) {
        case StpMode::None: return 'N';
        case StpMode::CancelResting: return 'R';
        case StpMode::CancelAggressor: return 'A';
        case StpMode::CancelBoth: return 'B';
    }
    return '?';
}
struct SequenceReset {
    char gap_fill{};
    std::uint64_t new_sequence{};
};
inline std::size_t encode(const SequenceReset& m, const Header& h, char* buffer, std::size_t capacity) noexcept {
    Builder b({buffer, capacity});
    b.header('4', h);
    b.field(123, m.gap_fill);
    b.uint_field(36, m.new_sequence);
    return b.finish();
}
struct NewOrderSingle {
    bool post_only() const noexcept { return has_exec_inst && exec_inst.view().find('6') != std::string_view::npos; }
    Text<> client_order_id{};
    Text<> account{};
    bool has_account = false;
    Text<> symbol{};
    Side side{};
    Quantity quantity{};
    OrdType order_type{};
    Price price{};
    bool has_price = false;
    TimeInForce time_in_force{};
    Text<> exec_inst{};
    bool has_exec_inst = false;
    Timestamp transact_time{};
    StpMode stp_mode{};
};
inline std::size_t encode(const NewOrderSingle& m, const Header& h, char* buffer, std::size_t capacity) noexcept {
    Builder b({buffer, capacity});
    b.header('D', h);
    b.field(11, m.client_order_id.view());
    if (m.has_account) b.field(1, m.account.view());
    b.field(55, m.symbol.view());
    b.field(54, (m.side == Side::BUY ? '1' : '2'));
    b.uint_field(38, m.quantity);
    b.field(40, (m.order_type == OrdType::Market ? '1' : '2'));
    if (m.has_price) b.price_field(44, m.price);
    b.field(59, tif_char(m.time_in_force));
    if (m.has_exec_inst) b.field(18, m.exec_inst.view());
    b.timestamp_field(60, m.transact_time);
    b.field(7928, stp_char(m.stp_mode));
    return b.finish();
}
struct OrderCancelRequest {
    Text<> original_client_order_id{};
    Text<> client_order_id{};
    Text<> symbol{};
    Side side{};
    Timestamp transact_time{};
};
inline std::size_t encode(const OrderCancelRequest& m, const Header& h, char* buffer, std::size_t capacity) noexcept {
    Builder b({buffer, capacity});
    b.header('F', h);
    b.field(41, m.original_client_order_id.view());
    b.field(11, m.client_order_id.view());
    b.field(55, m.symbol.view());
    b.field(54, (m.side == Side::BUY ? '1' : '2'));
    b.timestamp_field(60, m.transact_time);
    return b.finish();
}
struct OrderCancelReplaceRequest {
    Text<> original_client_order_id{};
    Text<> client_order_id{};
    Text<> symbol{};
    Side side{};
    Quantity quantity{};
    OrdType order_type{};
    Price price{};
    bool has_price = false;
    Timestamp transact_time{};
};
inline std::size_t encode(const OrderCancelReplaceRequest& m, const Header& h, char* buffer,
                          std::size_t capacity) noexcept {
    Builder b({buffer, capacity});
    b.header('G', h);
    b.field(41, m.original_client_order_id.view());
    b.field(11, m.client_order_id.view());
    b.field(55, m.symbol.view());
    b.field(54, (m.side == Side::BUY ? '1' : '2'));
    b.uint_field(38, m.quantity);
    b.field(40, (m.order_type == OrdType::Market ? '1' : '2'));
    if (m.has_price) b.price_field(44, m.price);
    b.timestamp_field(60, m.transact_time);
    return b.finish();
}
struct OrderMassCancelRequest {
    Text<> client_order_id{};
    char request_type{};
    Text<> symbol{};
    bool has_symbol = false;
};
inline std::size_t encode(const OrderMassCancelRequest& m, const Header& h, char* buffer,
                          std::size_t capacity) noexcept {
    Builder b({buffer, capacity});
    b.header('q', h);
    b.field(11, m.client_order_id.view());
    b.field(530, m.request_type);
    if (m.has_symbol) b.field(55, m.symbol.view());
    return b.finish();
}
}  // namespace exsim::protocol::fix44
#endif
