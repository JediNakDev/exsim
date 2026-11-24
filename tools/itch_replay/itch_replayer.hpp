#ifndef EXSIM_ITCH_REPLAY_ITCH_REPLAYER_HPP
#define EXSIM_ITCH_REPLAY_ITCH_REPLAYER_HPP

// Drives the matching engine from an ITCH 5.0 message stream through
// MatchingEngine::handle, the same entry point the exchange core uses.
//
// ITCH messages are post-matching state: an 'A'/'F' add never trades, so it
// is sent as a post-only order and a crossing add is rejected instead of
// matched. 'E'/'C'/'X' reduce the resting size, 'D' cancels the order and 'U'
// replaces it with a new reference and loses priority. The ITCH order
// reference is the client token, so the engine's own (session, token) map
// finds the order.
//
// The replay builds one book. Given a stock, it takes that stock's locate
// from its 'R' (Stock Directory) message and skips book messages for every
// other locate; without one, every book message goes to the book.

#include "reference_book.hpp"
#include <engine/matching_engine.hpp>
#include <protocol/itch50.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>

namespace exsim::itch_replay {

// Walks a NASDAQ ITCH 5.0 file, where each message follows its 2-byte
// big-endian length, and calls on_message(message) for each one in order.
// Returns false when the file ends inside a frame.
template <typename OnMessage>
bool for_each_message(std::span<const char> file, OnMessage&& on_message) {
    std::size_t offset = 0;
    while (offset < file.size()) {
        if (file.size() - offset < 2) return false;
        const std::size_t length = protocol::wire::load<2>(file.data() + offset);
        offset += 2;
        if (file.size() - offset < length) return false;
        on_message(file.subspan(offset, length));
        offset += length;
    }
    return true;
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
    std::uint64_t total() const noexcept { return adds + executes + cancels + deletes + replaces; }
};

class ItchReplayer {
public:
    explicit ItchReplayer(std::size_t max_live_orders, ReferenceBook* validate = nullptr,
                          std::optional<Symbol> stock = std::nullopt)
        : engine_(EngineConfig{.max_instruments = 1, .max_orders = max_live_orders}),
          events_(required_event_capacity({.max_instruments = 1, .max_orders = max_live_orders})),
          reference_(validate),
          stock_(stock) {
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

    // Applies one ITCH 5.0 message. Returns true for a book message (A, F, E,
    // C, X, D, U) of the replayed stock that decodes; false for any other
    // message, which changes nothing.
    bool apply(std::span<const char> message) {
        namespace itch50 = protocol::itch50;
        if (message.empty()) return false;
        switch (message[0]) {
            case itch50::StockDirectory::type: {
                itch50::StockDirectory m;
                if (stock_ && itch50::decode(message, m) == protocol::Status::ok && m.stock == *stock_)
                    locate_ = m.stock_locate;
                return false;
            }
            case itch50::AddOrder::type: {
                itch50::AddOrder m;
                if (!decode_for_stock(message, m)) return false;
                add(m.order_reference, m.side, m.price, m.shares);
                return true;
            }
            case itch50::AddOrderMPID::type: {
                itch50::AddOrderMPID m;
                if (!decode_for_stock(message, m)) return false;
                add(m.order_reference, m.side, m.price, m.shares);
                return true;
            }
            case itch50::OrderExecuted::type: {
                itch50::OrderExecuted m;
                if (!decode_for_stock(message, m)) return false;
                reduce(m.order_reference, m.shares);
                ++stats_.executes;
                return true;
            }
            case itch50::OrderExecutedWithPrice::type: {
                itch50::OrderExecutedWithPrice m;
                if (!decode_for_stock(message, m)) return false;
                reduce(m.order_reference, m.shares);
                ++stats_.executes;
                return true;
            }
            case itch50::OrderCancel::type: {
                itch50::OrderCancel m;
                if (!decode_for_stock(message, m)) return false;
                reduce(m.order_reference, m.shares);
                ++stats_.cancels;
                return true;
            }
            case itch50::OrderDelete::type: {
                itch50::OrderDelete m;
                if (!decode_for_stock(message, m)) return false;
                reduce(m.order_reference, std::numeric_limits<Quantity>::max());
                ++stats_.deletes;
                return true;
            }
            case itch50::OrderReplace::type: {
                itch50::OrderReplace m;
                if (!decode_for_stock(message, m)) return false;
                replace(m.original_reference, m.new_reference, m.price, m.shares);
                return true;
            }
            default: return false;
        }
    }

    [[nodiscard]] const MatchingEngine& engine() const noexcept { return engine_; }
    [[nodiscard]] InstrumentId instrument() const noexcept { return instrument_; }
    [[nodiscard]] const ReplayStats& stats() const noexcept { return stats_; }

private:
    static constexpr SessionId kSession = 1;

    // Decodes a book message; false when it does not decode or is for another stock.
    template <typename Message>
    bool decode_for_stock(std::span<const char> bytes, Message& m) const noexcept {
        return protocol::itch50::decode(bytes, m) == protocol::Status::ok &&
               (!stock_ || (locate_ != 0 && m.stock_locate == locate_));
    }

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
    std::optional<Symbol> stock_;
    std::uint16_t locate_ = 0;  // the stock's locate once its R arrives; book messages never use 0
    ReplayStats stats_;
};

}  // namespace exsim::itch_replay

#endif
