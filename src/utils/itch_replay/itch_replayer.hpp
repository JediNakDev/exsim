#ifndef EXSIM_ITCH_REPLAY_ITCH_REPLAYER_HPP
#define EXSIM_ITCH_REPLAY_ITCH_REPLAYER_HPP

// Drives the matching engine from an ITCH 5.0 message stream through
// MatchingEngine::handle, the same entry point the exchange core uses.
//
// ITCH messages are post-matching state: an 'A'/'F' add never trades, so it
// is sent as a post-only order and a crossing add is rejected instead of
// matched. 'E'/'X' reduce the resting size, 'D' cancels the order and 'U'
// replaces it with a new reference and loses priority. The ITCH order
// reference is the client token, so the engine's own (session, token) map
// finds the order. All messages go to one instrument.

#include "reference_book.hpp"
#include <engine/matching_engine.hpp>
#include <protocol/itch50.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace exsim::itch_replay {

// Wire size of the ITCH 5.0 messages that change the book (A, F, E, X, D,
// U), or 0 for any other type byte.
[[nodiscard]] inline std::size_t book_message_size(char type) noexcept {
    namespace itch50 = protocol::itch50;
    switch (type) {
    case itch50::AddOrder::type: return itch50::AddOrder::wire_size;
    case itch50::AddOrderMPID::type: return itch50::AddOrderMPID::wire_size;
    case itch50::OrderExecuted::type: return itch50::OrderExecuted::wire_size;
    case itch50::OrderCancel::type: return itch50::OrderCancel::wire_size;
    case itch50::OrderDelete::type: return itch50::OrderDelete::wire_size;
    case itch50::OrderReplace::type: return itch50::OrderReplace::wire_size;
    default: return 0;
    }
}

// Every live order comes from an add message, so the stream size bounds the
// book. The cap keeps a full-day file within a few gigabytes; adds beyond it
// are rejected and counted.
[[nodiscard]] inline std::size_t max_live_orders_for(std::size_t stream_bytes) noexcept {
    constexpr std::size_t kMaxLiveOrders = std::size_t{1} << 22;
    const std::size_t add_messages = stream_bytes / protocol::itch50::AddOrder::wire_size + 1;
    return add_messages < kMaxLiveOrders ? add_messages : kMaxLiveOrders;
}

struct ReplayStats {
    std::uint64_t adds = 0;
    std::uint64_t executes = 0;
    std::uint64_t cancels = 0;
    std::uint64_t deletes = 0;
    std::uint64_t replaces = 0;
    std::uint64_t unknown_ref = 0;
    std::uint64_t dropped_cross = 0;  // adds and replaces that would have crossed the book
    std::uint64_t rejected = 0;       // any other engine rejection (price band, capacity, ...)
    std::uint64_t total() const noexcept {
        return adds + executes + cancels + deletes + replaces;
    }
};

class ItchReplayer {
public:
    explicit ItchReplayer(std::size_t max_live_orders, ReferenceBook* validate = nullptr)
        : engine_(EngineConfig{.max_instruments = 1, .max_orders = max_live_orders}),
          events_(required_event_capacity({.max_instruments = 1, .max_orders = max_live_orders})),
          reference_(validate) {
        // ITCH prices have 4 decimals: one tick is $0.0001 and the band ends at $200.
        InstrumentConfig config;
        config.symbol = Symbol("ITCH");
        config.tick_size = 1;
        config.max_order_qty = std::numeric_limits<Quantity>::max();
        config.min_price = 1;
        config.max_price = 2'000'000;
        instrument_ = engine_.add_instrument(config);
        if (instrument_ == 0) throw std::runtime_error("replay instrument rejected");
    }

    // Decodes one ITCH 5.0 message (exactly book_message_size(type) bytes)
    // and applies it. Returns false for a message that does not decode.
    bool apply(std::span<const char> message) {
        namespace itch50 = protocol::itch50;
        switch (message[0]) {
        case itch50::AddOrder::type: {
            itch50::AddOrder m;
            if (itch50::decode(message, m) != protocol::Status::ok) return false;
            add(m.order_reference, m.side, m.price, m.shares);
            return true;
        }
        case itch50::AddOrderMPID::type: {
            itch50::AddOrderMPID m;
            if (itch50::decode(message, m) != protocol::Status::ok) return false;
            add(m.order_reference, m.side, m.price, m.shares);
            return true;
        }
        case itch50::OrderExecuted::type: {
            itch50::OrderExecuted m;
            if (itch50::decode(message, m) != protocol::Status::ok) return false;
            reduce(m.order_reference, m.shares);
            ++stats_.executes;
            return true;
        }
        case itch50::OrderCancel::type: {
            itch50::OrderCancel m;
            if (itch50::decode(message, m) != protocol::Status::ok) return false;
            reduce(m.order_reference, m.shares);
            ++stats_.cancels;
            return true;
        }
        case itch50::OrderDelete::type: {
            itch50::OrderDelete m;
            if (itch50::decode(message, m) != protocol::Status::ok) return false;
            reduce(m.order_reference, std::numeric_limits<Quantity>::max());
            ++stats_.deletes;
            return true;
        }
        case itch50::OrderReplace::type: {
            itch50::OrderReplace m;
            if (itch50::decode(message, m) != protocol::Status::ok) return false;
            replace(m.original_reference, m.new_reference, m.price, m.shares);
            return true;
        }
        default:
            return false;
        }
    }

    [[nodiscard]] const MatchingEngine& engine() const noexcept { return engine_; }
    [[nodiscard]] InstrumentId instrument() const noexcept { return instrument_; }
    [[nodiscard]] const ReplayStats& stats() const noexcept { return stats_; }

private:
    static constexpr SessionId kSession = 1;

    // Runs one command. Returns false when the engine rejected it, after
    // counting the rejection.
    template <typename CommandT>
    bool submit(const CommandT& command) {
        events_.clear();
        if (engine_.handle(command, events_) != HandleResult::Applied) {
            throw std::runtime_error("replay event buffer too small");
        }
        // A zero-share reduction changes nothing and emits no event.
        if (events_.empty() || events_[0].type != EventType::Rejected) return true;
        switch (events_[0].rejected.reason) {
        case RejectReason::UnknownOrder: ++stats_.unknown_ref; break;
        case RejectReason::PostOnlyWouldCross: ++stats_.dropped_cross; break;
        default: ++stats_.rejected; break;
        }
        return false;
    }

    void add(std::uint64_t ref, Side side, Price price, Quantity shares) {
        ++stats_.adds;
        NewOrder order;
        order.instrument = instrument_;
        order.session = kSession;
        order.cl_ord_id = ref;
        order.side = side;
        order.tif = TimeInForce::GTC;
        order.post_only = true;
        order.price = price;
        order.qty = shares;
        if (submit(order) && reference_) reference_->add(ref, side, price, shares);
    }

    // Removes `shares` from the order, or the whole order when it has no more.
    void reduce(std::uint64_t ref, Quantity shares) {
        const auto order = engine_.find_order(kSession, ref);
        CancelOrder cancel;
        cancel.session = kSession;
        cancel.orig_cl_ord_id = ref;
        // Replayed orders never trade, so total quantity equals leaves.
        if (order && shares < order->leaves_qty) cancel.reduce_to_total_qty = order->leaves_qty - shares;
        if (submit(cancel) && reference_) reference_->reduce(ref, shares);
    }

    void replace(std::uint64_t old_ref, std::uint64_t new_ref, Price price, Quantity shares) {
        ++stats_.replaces;
        ReplaceOrder command;
        command.session = kSession;
        command.orig_cl_ord_id = old_ref;
        command.cl_ord_id = new_ref;
        command.price = price;
        command.qty = shares;
        command.force_priority_loss = true;
        if (submit(command) && reference_) reference_->replace(old_ref, new_ref, price, shares);
    }

    MatchingEngine engine_;
    EventBuffer events_;
    ReferenceBook* reference_;
    InstrumentId instrument_ = 0;
    ReplayStats stats_;
};

}  // namespace exsim::itch_replay

#endif
