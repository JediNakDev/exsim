#pragma once
#include <protocol/fix44.hpp>
namespace exsim::protocol::fix44 {
template <std::size_t N>
Status decode(const Parser<N>& p, Logon& m) noexcept {
    if (p.get(35) != "A") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    if (!p.has(98)) return Status::missing_field;
    {
        if (!p.get_uint(98, m.encrypt_method)) return Status::invalid_field;
    }
    if (!p.has(108)) return Status::missing_field;
    {
        if (!p.get_uint(108, m.heartbeat_interval)) return Status::invalid_field;
    }
    m.has_reset_sequence = p.has(141);
    if (m.has_reset_sequence) {
        if (!p.get_char(141, m.reset_sequence) ||
            std::string_view("YN").find(m.reset_sequence) == std::string_view::npos)
            return Status::invalid_field;
    }
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, Logon& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
template <std::size_t N>
Status decode(const Parser<N>& p, Heartbeat& m) noexcept {
    if (p.get(35) != "0") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    m.has_test_request_id = p.has(112);
    if (m.has_test_request_id) {
        m.test_request_id.borrow(p.get(112));
        if (m.test_request_id.view().empty()) return Status::invalid_field;
    }
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, Heartbeat& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
template <std::size_t N>
Status decode(const Parser<N>& p, TestRequest& m) noexcept {
    if (p.get(35) != "1") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    if (!p.has(112)) return Status::missing_field;
    {
        m.test_request_id.borrow(p.get(112));
        if (m.test_request_id.view().empty()) return Status::invalid_field;
    }
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, TestRequest& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
template <std::size_t N>
Status decode(const Parser<N>& p, ResendRequest& m) noexcept {
    if (p.get(35) != "2") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    if (!p.has(7)) return Status::missing_field;
    {
        if (!p.get_uint(7, m.begin_sequence)) return Status::invalid_field;
    }
    if (!p.has(16)) return Status::missing_field;
    {
        if (!p.get_uint(16, m.end_sequence)) return Status::invalid_field;
    }
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, ResendRequest& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
template <std::size_t N>
Status decode(const Parser<N>& p, Reject& m) noexcept {
    if (p.get(35) != "3") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    if (!p.has(45)) return Status::missing_field;
    {
        if (!p.get_uint(45, m.reference_sequence)) return Status::invalid_field;
    }
    if (!p.has(371)) return Status::missing_field;
    {
        if (!p.get_uint(371, m.reference_tag)) return Status::invalid_field;
    }
    if (!p.has(372)) return Status::missing_field;
    {
        m.reference_type.borrow(p.get(372));
        if (m.reference_type.view().empty()) return Status::invalid_field;
    }
    if (!p.has(373)) return Status::missing_field;
    {
        if (!p.get_uint(373, m.reason)) return Status::invalid_field;
    }
    if (!p.has(58)) return Status::missing_field;
    {
        m.text.borrow(p.get(58));
        if (m.text.view().empty()) return Status::invalid_field;
    }
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, Reject& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
template <std::size_t N>
Status decode(const Parser<N>& p, SequenceReset& m) noexcept {
    if (p.get(35) != "4") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    if (!p.has(123)) return Status::missing_field;
    {
        if (!p.get_char(123, m.gap_fill) || std::string_view("YN").find(m.gap_fill) == std::string_view::npos)
            return Status::invalid_field;
    }
    if (!p.has(36)) return Status::missing_field;
    {
        if (!p.get_uint(36, m.new_sequence)) return Status::invalid_field;
    }
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, SequenceReset& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
template <std::size_t N>
Status decode(const Parser<N>& p, Logout& m) noexcept {
    if (p.get(35) != "5") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    m.has_text = p.has(58);
    if (m.has_text) {
        m.text.borrow(p.get(58));
        if (m.text.view().empty()) return Status::invalid_field;
    }
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, Logout& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
template <std::size_t N>
Status decode(const Parser<N>& p, NewOrderSingle& m) noexcept {
    if (p.get(35) != "D") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    if (!p.has(11)) return Status::missing_field;
    {
        m.client_order_id.borrow(p.get(11));
        if (m.client_order_id.view().empty()) return Status::invalid_field;
    }
    m.has_account = p.has(1);
    if (m.has_account) {
        m.account.borrow(p.get(1));
        if (m.account.view().empty()) return Status::invalid_field;
    }
    if (!p.has(55)) return Status::missing_field;
    {
        m.symbol.borrow(p.get(55));
        if (m.symbol.view().empty()) return Status::invalid_field;
    }
    if (!p.has(54)) return Status::missing_field;
    {
        char c;
        if (!p.get_char(54, c)) return Status::invalid_field;
        switch (c) {
            case '1': m.side = Side::BUY; break;
            case '2': m.side = Side::SELL; break;
            default: return Status::invalid_field;
        }
    }
    if (!p.has(38)) return Status::missing_field;
    {
        std::uint64_t value;
        if (!p.get_uint(38, value) || value > UINT32_MAX) return Status::invalid_field;
        m.quantity = static_cast<Quantity>(value);
    }
    if (!p.has(40)) return Status::missing_field;
    {
        char c;
        if (!p.get_char(40, c)) return Status::invalid_field;
        switch (c) {
            case '1': m.order_type = OrdType::Market; break;
            case '2': m.order_type = OrdType::Limit; break;
            default: return Status::invalid_field;
        }
    }
    m.has_price = p.has(44);
    if (m.has_price) {
        if (!p.get_price(44, m.price)) return Status::invalid_field;
    }
    if (!p.has(59)) return Status::missing_field;
    {
        char c;
        if (!p.get_char(59, c)) return Status::invalid_field;
        switch (c) {
            case '0': m.time_in_force = TimeInForce::Day; break;
            case '1': m.time_in_force = TimeInForce::GTC; break;
            case '3': m.time_in_force = TimeInForce::IOC; break;
            case '4': m.time_in_force = TimeInForce::FOK; break;
            default: return Status::invalid_field;
        }
    }
    m.has_exec_inst = p.has(18);
    if (m.has_exec_inst) {
        m.exec_inst.borrow(p.get(18));
        if (m.exec_inst.view().empty()) return Status::invalid_field;
    }
    if (!p.has(60)) return Status::missing_field;
    {
        if (!p.get_timestamp(60, m.transact_time)) return Status::invalid_field;
    }
    if (!p.has(7928)) return Status::missing_field;
    {
        char c;
        if (!p.get_char(7928, c)) return Status::invalid_field;
        switch (c) {
            case 'N': m.stp_mode = StpMode::None; break;
            case 'R': m.stp_mode = StpMode::CancelResting; break;
            case 'A': m.stp_mode = StpMode::CancelAggressor; break;
            case 'B': m.stp_mode = StpMode::CancelBoth; break;
            default: return Status::invalid_field;
        }
    }
    if (m.order_type == OrdType::Limit && !m.has_price) return Status::missing_field;
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, NewOrderSingle& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
template <std::size_t N>
Status decode(const Parser<N>& p, OrderCancelRequest& m) noexcept {
    if (p.get(35) != "F") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    if (!p.has(41)) return Status::missing_field;
    {
        m.original_client_order_id.borrow(p.get(41));
        if (m.original_client_order_id.view().empty()) return Status::invalid_field;
    }
    if (!p.has(11)) return Status::missing_field;
    {
        m.client_order_id.borrow(p.get(11));
        if (m.client_order_id.view().empty()) return Status::invalid_field;
    }
    if (!p.has(55)) return Status::missing_field;
    {
        m.symbol.borrow(p.get(55));
        if (m.symbol.view().empty()) return Status::invalid_field;
    }
    if (!p.has(54)) return Status::missing_field;
    {
        char c;
        if (!p.get_char(54, c)) return Status::invalid_field;
        switch (c) {
            case '1': m.side = Side::BUY; break;
            case '2': m.side = Side::SELL; break;
            default: return Status::invalid_field;
        }
    }
    if (!p.has(60)) return Status::missing_field;
    {
        if (!p.get_timestamp(60, m.transact_time)) return Status::invalid_field;
    }
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, OrderCancelRequest& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
template <std::size_t N>
Status decode(const Parser<N>& p, OrderCancelReplaceRequest& m) noexcept {
    if (p.get(35) != "G") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    if (!p.has(41)) return Status::missing_field;
    {
        m.original_client_order_id.borrow(p.get(41));
        if (m.original_client_order_id.view().empty()) return Status::invalid_field;
    }
    if (!p.has(11)) return Status::missing_field;
    {
        m.client_order_id.borrow(p.get(11));
        if (m.client_order_id.view().empty()) return Status::invalid_field;
    }
    if (!p.has(55)) return Status::missing_field;
    {
        m.symbol.borrow(p.get(55));
        if (m.symbol.view().empty()) return Status::invalid_field;
    }
    if (!p.has(54)) return Status::missing_field;
    {
        char c;
        if (!p.get_char(54, c)) return Status::invalid_field;
        switch (c) {
            case '1': m.side = Side::BUY; break;
            case '2': m.side = Side::SELL; break;
            default: return Status::invalid_field;
        }
    }
    if (!p.has(38)) return Status::missing_field;
    {
        std::uint64_t value;
        if (!p.get_uint(38, value) || value > UINT32_MAX) return Status::invalid_field;
        m.quantity = static_cast<Quantity>(value);
    }
    if (!p.has(40)) return Status::missing_field;
    {
        char c;
        if (!p.get_char(40, c)) return Status::invalid_field;
        switch (c) {
            case '1': m.order_type = OrdType::Market; break;
            case '2': m.order_type = OrdType::Limit; break;
            default: return Status::invalid_field;
        }
    }
    m.has_price = p.has(44);
    if (m.has_price) {
        if (!p.get_price(44, m.price)) return Status::invalid_field;
    }
    if (!p.has(60)) return Status::missing_field;
    {
        if (!p.get_timestamp(60, m.transact_time)) return Status::invalid_field;
    }
    if (m.order_type == OrdType::Limit && !m.has_price) return Status::missing_field;
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, OrderCancelReplaceRequest& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
template <std::size_t N>
Status decode(const Parser<N>& p, OrderMassCancelRequest& m) noexcept {
    if (p.get(35) != "q") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    if (!p.has(11)) return Status::missing_field;
    {
        m.client_order_id.borrow(p.get(11));
        if (m.client_order_id.view().empty()) return Status::invalid_field;
    }
    if (!p.has(530)) return Status::missing_field;
    {
        if (!p.get_char(530, m.request_type) || std::string_view("17").find(m.request_type) == std::string_view::npos)
            return Status::invalid_field;
    }
    m.has_symbol = p.has(55);
    if (m.has_symbol) {
        m.symbol.borrow(p.get(55));
        if (m.symbol.view().empty()) return Status::invalid_field;
    }
    if (m.request_type != '1' && m.request_type != '7') return Status::invalid_field;
    if (m.request_type == '1' && !m.has_symbol) return Status::missing_field;
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, OrderMassCancelRequest& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
template <std::size_t N>
Status decode(const Parser<N>& p, ExecutionReport& m) noexcept {
    if (p.get(35) != "8") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    if (!p.has(37)) return Status::missing_field;
    {
        m.order_id.borrow(p.get(37));
        if (m.order_id.view().empty()) return Status::invalid_field;
    }
    if (!p.has(11)) return Status::missing_field;
    {
        m.client_order_id.borrow(p.get(11));
        if (m.client_order_id.view().empty()) return Status::invalid_field;
    }
    m.has_original_client_order_id = p.has(41);
    if (m.has_original_client_order_id) {
        m.original_client_order_id.borrow(p.get(41));
        if (m.original_client_order_id.view().empty()) return Status::invalid_field;
    }
    if (!p.has(17)) return Status::missing_field;
    {
        m.execution_id.borrow(p.get(17));
        if (m.execution_id.view().empty()) return Status::invalid_field;
    }
    if (!p.has(150)) return Status::missing_field;
    {
        if (!p.get_char(150, m.execution_type) ||
            std::string_view("0458FC").find(m.execution_type) == std::string_view::npos)
            return Status::invalid_field;
    }
    if (!p.has(39)) return Status::missing_field;
    {
        if (!p.get_char(39, m.order_status) ||
            std::string_view("01248C").find(m.order_status) == std::string_view::npos)
            return Status::invalid_field;
    }
    if (!p.has(55)) return Status::missing_field;
    {
        m.symbol.borrow(p.get(55));
        if (m.symbol.view().empty()) return Status::invalid_field;
    }
    if (!p.has(54)) return Status::missing_field;
    {
        char c;
        if (!p.get_char(54, c)) return Status::invalid_field;
        switch (c) {
            case '1': m.side = Side::BUY; break;
            case '2': m.side = Side::SELL; break;
            default: return Status::invalid_field;
        }
    }
    if (!p.has(38)) return Status::missing_field;
    {
        std::uint64_t value;
        if (!p.get_uint(38, value) || value > UINT32_MAX) return Status::invalid_field;
        m.quantity = static_cast<Quantity>(value);
    }
    m.has_price = p.has(44);
    if (m.has_price) {
        if (!p.get_price(44, m.price)) return Status::invalid_field;
    }
    if (!p.has(32)) return Status::missing_field;
    {
        std::uint64_t value;
        if (!p.get_uint(32, value) || value > UINT32_MAX) return Status::invalid_field;
        m.last_quantity = static_cast<Quantity>(value);
    }
    if (!p.has(31)) return Status::missing_field;
    {
        if (!p.get_price(31, m.last_price)) return Status::invalid_field;
    }
    if (!p.has(151)) return Status::missing_field;
    {
        std::uint64_t value;
        if (!p.get_uint(151, value) || value > UINT32_MAX) return Status::invalid_field;
        m.leaves_quantity = static_cast<Quantity>(value);
    }
    if (!p.has(14)) return Status::missing_field;
    {
        std::uint64_t value;
        if (!p.get_uint(14, value) || value > UINT32_MAX) return Status::invalid_field;
        m.cumulative_quantity = static_cast<Quantity>(value);
    }
    if (!p.has(6)) return Status::missing_field;
    {
        if (!p.get_price(6, m.average_price)) return Status::invalid_field;
    }
    m.has_reject_reason = p.has(103);
    if (m.has_reject_reason) {
        if (!p.get_uint(103, m.reject_reason)) return Status::invalid_field;
    }
    m.has_text = p.has(58);
    if (m.has_text) {
        m.text.borrow(p.get(58));
        if (m.text.view().empty()) return Status::invalid_field;
    }
    if (!p.has(60)) return Status::missing_field;
    {
        if (!p.get_timestamp(60, m.transact_time)) return Status::invalid_field;
    }
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, ExecutionReport& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
template <std::size_t N>
Status decode(const Parser<N>& p, OrderCancelReject& m) noexcept {
    if (p.get(35) != "9") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    if (!p.has(37)) return Status::missing_field;
    {
        m.order_id.borrow(p.get(37));
        if (m.order_id.view().empty()) return Status::invalid_field;
    }
    if (!p.has(11)) return Status::missing_field;
    {
        m.client_order_id.borrow(p.get(11));
        if (m.client_order_id.view().empty()) return Status::invalid_field;
    }
    if (!p.has(41)) return Status::missing_field;
    {
        m.original_client_order_id.borrow(p.get(41));
        if (m.original_client_order_id.view().empty()) return Status::invalid_field;
    }
    if (!p.has(39)) return Status::missing_field;
    {
        if (!p.get_char(39, m.order_status) ||
            std::string_view("01248C").find(m.order_status) == std::string_view::npos)
            return Status::invalid_field;
    }
    if (!p.has(434)) return Status::missing_field;
    {
        if (!p.get_char(434, m.response_to) || std::string_view("12").find(m.response_to) == std::string_view::npos)
            return Status::invalid_field;
    }
    if (!p.has(102)) return Status::missing_field;
    {
        if (!p.get_uint(102, m.reason)) return Status::invalid_field;
    }
    m.has_text = p.has(58);
    if (m.has_text) {
        m.text.borrow(p.get(58));
        if (m.text.view().empty()) return Status::invalid_field;
    }
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, OrderCancelReject& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
template <std::size_t N>
Status decode(const Parser<N>& p, OrderMassCancelReport& m) noexcept {
    if (p.get(35) != "r") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    if (!p.has(11)) return Status::missing_field;
    {
        m.client_order_id.borrow(p.get(11));
        if (m.client_order_id.view().empty()) return Status::invalid_field;
    }
    if (!p.has(37)) return Status::missing_field;
    {
        m.order_id.borrow(p.get(37));
        if (m.order_id.view().empty()) return Status::invalid_field;
    }
    if (!p.has(530)) return Status::missing_field;
    {
        if (!p.get_char(530, m.request_type) ||
            std::string_view("1234567").find(m.request_type) == std::string_view::npos)
            return Status::invalid_field;
    }
    if (!p.has(531)) return Status::missing_field;
    {
        if (!p.get_char(531, m.response) || std::string_view("01234567").find(m.response) == std::string_view::npos)
            return Status::invalid_field;
    }
    m.has_reject_reason = p.has(532);
    if (m.has_reject_reason) {
        if (!p.get_uint(532, m.reject_reason)) return Status::invalid_field;
    }
    if (!p.has(533)) return Status::missing_field;
    {
        if (!p.get_uint(533, m.total_affected_orders)) return Status::invalid_field;
    }
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, OrderMassCancelReport& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
template <std::size_t N>
Status decode(const Parser<N>& p, BusinessMessageReject& m) noexcept {
    if (p.get(35) != "j") return Status::invalid_type;
    if (!p.has(49) || !p.has(56) || !p.has(34) || !p.has(52)) return Status::missing_field;
    std::uint64_t header_value;
    if (!p.get_uint(34, header_value) || !p.get_timestamp(52, header_value)) return Status::invalid_field;
    m.has_reference_sequence = p.has(45);
    if (m.has_reference_sequence) {
        if (!p.get_uint(45, m.reference_sequence)) return Status::invalid_field;
    }
    if (!p.has(372)) return Status::missing_field;
    {
        m.reference_type.borrow(p.get(372));
        if (m.reference_type.view().empty()) return Status::invalid_field;
    }
    m.has_business_reject_reference_id = p.has(379);
    if (m.has_business_reject_reference_id) {
        m.business_reject_reference_id.borrow(p.get(379));
        if (m.business_reject_reference_id.view().empty()) return Status::invalid_field;
    }
    if (!p.has(380)) return Status::missing_field;
    {
        if (!p.get_uint(380, m.reason)) return Status::invalid_field;
    }
    m.has_text = p.has(58);
    if (m.has_text) {
        m.text.borrow(p.get(58));
        if (m.text.view().empty()) return Status::invalid_field;
    }
    return Status::ok;
}
inline Status decode(const char* p, std::size_t n, BusinessMessageReject& m) noexcept {
    Parser<> parser;
    auto s = parser.parse(p, n);
    return s == Status::ok ? decode(parser, m) : s;
}
}
