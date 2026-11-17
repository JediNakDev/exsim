#include <book/order_book.hpp>
#include <cassert>

namespace exsim::book {

// Every live level holds at least one live order, so max_orders bounds both pools.
OrderBookStorage::OrderBookStorage(std::size_t max_orders)
    : orders(max_orders), order_pool(max_orders), level_pool(max_orders) {}

OrderBook::OrderBook(std::uint32_t tick_count, OrderBookStorage& storage)
    : storage_(storage)
    , bid_ladder_(tick_count, nullptr)
    , ask_ladder_(tick_count, nullptr)
    , bid_active_levels_(tick_count)
    , ask_active_levels_(tick_count) {}

Order* OrderBook::rest_order(OrderId id, std::int32_t tick, Quantity leaves, Side side,
                             std::uint32_t context_index) {
    Order* const order = storage_.order_pool.create();
    if (!order) return nullptr;
    // The insert is the only hash probe: it reports a duplicate id itself.
    if (!storage_.orders.insert(id, order)) {
        storage_.order_pool.destroy(order);
        return nullptr;
    }
    order->id = id;
    order->tick = tick;
    order->remaining_quantity = leaves;
    order->context_index = context_index;
    order->side = side;

    const std::size_t index = static_cast<std::size_t>(tick);
    PriceLevel*& level = (side == Side::BUY ? bid_ladder_ : ask_ladder_)[index];
    if (!level) {
        level = storage_.level_pool.create(tick);
        assert(level);  // there are never more levels than orders
        if (side == Side::BUY) {
            bid_active_levels_.set(index);
            if (tick > best_bid_tick_) best_bid_tick_ = tick;
        } else {
            ask_active_levels_.set(index);
            if (tick < best_ask_tick_) best_ask_tick_ = tick;
        }
    }
    level->add_order(order);
    leaves_by_side_[static_cast<std::size_t>(side)] += leaves;
    return order;
}

void OrderBook::reduce_order(Order& order, Quantity quantity) noexcept {
    assert(quantity <= order.remaining_quantity);
    order.remaining_quantity -= quantity;
    order.parent_level->total_volume -= quantity;
    leaves_by_side_[static_cast<std::size_t>(order.side)] -= quantity;
}

void OrderBook::remove_order(Order& order) noexcept {
    PriceLevel* const level = order.parent_level;
    leaves_by_side_[static_cast<std::size_t>(order.side)] -= order.remaining_quantity;
    level->remove_order(&order);
    if (level->head_order == nullptr) {
        const std::int32_t tick = level->tick;
        const std::size_t index = static_cast<std::size_t>(tick);
        if (order.side == Side::BUY) {
            bid_ladder_[index] = nullptr;
            bid_active_levels_.clear(index);
            if (tick == best_bid_tick_) best_bid_tick_ = next_tick(Side::BUY, tick - 1, false).value_or(kNoBidTick);
        } else {
            ask_ladder_[index] = nullptr;
            ask_active_levels_.clear(index);
            if (tick == best_ask_tick_) best_ask_tick_ = next_tick(Side::SELL, tick + 1, true).value_or(kNoAskTick);
        }
        storage_.level_pool.destroy(level);
    }
    (void)storage_.orders.erase(order.id);
    storage_.order_pool.destroy(&order);
}

std::optional<std::int32_t> OrderBook::next_tick(Side side, std::int32_t from, bool ascending) const noexcept {
    const ActiveBitmap& levels = side == Side::BUY ? bid_active_levels_ : ask_active_levels_;
    // A negative `from` becomes an index past the end, which finds nothing.
    const std::size_t start = static_cast<std::size_t>(from);
    const auto index = ascending ? levels.find_at_or_above(start) : levels.find_at_or_below(start);
    return index ? std::optional<std::int32_t>(static_cast<std::int32_t>(*index)) : std::nullopt;
}

}  // namespace exsim::book
