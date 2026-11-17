#ifndef EXSIM_ENGINE_COMMAND_HPP
#define EXSIM_ENGINE_COMMAND_HPP

// Inbound commands: the protocol-neutral representation every order-entry
// gateway (OUCH, FIX, ...) decodes into. All structs are trivially copyable
// for SPSC queues. Journals require explicit versioned field encoding, never
// raw object storage (padding, ABI and endianness are not canonical).

#include "engine/types.hpp"
#include <type_traits>

namespace exsim {

struct NewOrderData {
    InstrumentId instrument = 0;
    SessionId session = 0;
    OwnerId owner = 0;
    ClOrdId cl_ord_id = 0;
    Side side = Side::BUY;
    OrdType type = OrdType::Limit;
    TimeInForce tif = TimeInForce::Day;
    StpMode stp = StpMode::None;
    bool post_only = false;
    Price price = 0;  // ignored for market orders
    Quantity qty = 0;
};

struct CancelOrderData {
    SessionId session = 0;
    ClOrdId orig_cl_ord_id = 0;        // token of the live order to cancel
    ClOrdId cl_ord_id = 0;             // token of this request (echoed back; may be 0)
    Quantity reduce_to_total_qty = 0;  // 0 full cancel; else clamp leaves to max(total-cum,0)
};

struct ReplaceOrderData {
    SessionId session = 0;
    ClOrdId orig_cl_ord_id = 0;        // token of the live order to replace
    ClOrdId cl_ord_id = 0;             // new token for the order; 0 = keep orig
    Price price = 0;                   // new limit price
    Quantity qty = 0;                  // new TOTAL quantity (incl. already filled)
    bool force_priority_loss = false;  // OUCH U always sets true
};

struct MassCancelData {
    SessionId session = 0;
    InstrumentId instrument = 0;        // 0 = all instruments
    ClOrdId cl_ord_id = 0;              // token of this request (echoed back)
    bool cancel_on_disconnect = false;  // selects CancelReason
};

struct SetPhaseData {
    InstrumentId instrument = 0;  // 0 = all instruments
    Phase phase = Phase::Continuous;
};

// Gateways may submit a timestamped typed request directly. Command stores
// that timestamp once in its header and only the request data in its union.
template <typename Payload>
struct Timestamped : Payload {
    Timestamp ts = 0;
    Timestamped() = default;
    template <typename... Args>
    Timestamped(Timestamp stamp, Args... args) noexcept : Payload(args...), ts(stamp) {}
};

using NewOrder = Timestamped<NewOrderData>;
using CancelOrder = Timestamped<CancelOrderData>;
using ReplaceOrder = Timestamped<ReplaceOrderData>;
using MassCancel = Timestamped<MassCancelData>;
using SetPhase = Timestamped<SetPhaseData>;

// Explicit tags and unions keep queue records trivially copyable and avoid
// a second variant discriminator. Wire and journal formats encode fields.
struct Command {
    Timestamp timestamp = 0;
    CommandType type = CommandType::NewOrder;
    union {
        NewOrderData new_order;
        CancelOrderData cancel;
        ReplaceOrderData replace;
        MassCancelData mass_cancel;
        SetPhaseData set_phase;
    };

    Command() noexcept : new_order() {}
    Command(const NewOrder& c) noexcept
        : timestamp(c.ts), type(CommandType::NewOrder), new_order(static_cast<const NewOrderData&>(c)) {}
    Command(const CancelOrder& c) noexcept
        : timestamp(c.ts), type(CommandType::Cancel), cancel(static_cast<const CancelOrderData&>(c)) {}
    Command(const ReplaceOrder& c) noexcept
        : timestamp(c.ts), type(CommandType::Replace), replace(static_cast<const ReplaceOrderData&>(c)) {}
    Command(const MassCancel& c) noexcept
        : timestamp(c.ts), type(CommandType::MassCancel), mass_cancel(static_cast<const MassCancelData&>(c)) {}
    Command(const SetPhase& c) noexcept
        : timestamp(c.ts), type(CommandType::SetPhase), set_phase(static_cast<const SetPhaseData&>(c)) {}

    [[nodiscard]] Timestamp ts() const noexcept { return timestamp; }
};

static_assert(std::is_trivially_copyable_v<Command>);

}  // namespace exsim

#endif
