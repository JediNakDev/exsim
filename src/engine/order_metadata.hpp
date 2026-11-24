#ifndef EXSIM_ENGINE_ORDER_METADATA_HPP
#define EXSIM_ENGINE_ORDER_METADATA_HPP

// Engine-only order metadata indexed by the order book order's context_index.
// The order book owns resting orders (ID, price tick, leaves), price levels and
// FIFO links. One preallocated record per order holds what the book does not:
// session and STP attributes, cumulative fills, and the links of the
// per-session and per-instrument order lists. Index 0 means no metadata slot.

#include <common/types.hpp>
#include <book/order.hpp>

#include <cstdint>

namespace exsim::engine {

using OrderIndex = std::uint32_t;
inline constexpr OrderIndex kNilOrder = 0;

// (session << 48) | cl_ord_id: the key of the (session, token) map and the
// way FillLeg/Canceled recover both identifiers from one hot field.
inline constexpr unsigned kSessionTokenShift = 48;

[[nodiscard]] constexpr std::uint64_t make_session_token(SessionId session, ClOrdId cl_ord_id) noexcept {
    return (static_cast<std::uint64_t>(session) << kSessionTokenShift) | cl_ord_id;
}

[[nodiscard]] constexpr SessionId session_of(std::uint64_t session_token) noexcept {
    return static_cast<SessionId>(session_token >> kSessionTokenShift);
}

[[nodiscard]] constexpr ClOrdId cl_ord_id_of(std::uint64_t session_token) noexcept {
    return session_token & kMaxClOrdId;
}

// One cache line, so a rest, fill or cancel reads one metadata line per order.
struct alignas(64) OrderMetadata {
    exsim::book::Order* order;    // the resting book order; its context_index is this slot
    std::uint64_t session_token;  // make_session_token(session, cl_ord_id)
    Quantity cum_qty;
    OwnerId owner;
    OrderIndex next_free;  // free-list link while the slot is free
    InstrumentId instrument;
    TimeInForce tif;
    StpMode stp;
    bool post_only;
    OrderIndex next_in_session;  // per-session list, ascending OrderId
    OrderIndex prev_in_session;
    OrderIndex next_in_instrument;  // per-instrument list, ascending OrderId
    OrderIndex prev_in_instrument;
};
static_assert(sizeof(OrderMetadata) == 64, "one metadata record per cache line");

// Head and tail of an intrusive doubly linked list of orders.
struct OrderListEnds {
    OrderIndex head;
    OrderIndex tail;
};

}  // namespace exsim::engine

#endif
