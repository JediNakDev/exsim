#ifndef EXSIM_BOOK_ORDER_BOOK_HPP
#define EXSIM_BOOK_ORDER_BOOK_HPP

// Passive storage for the matching engine: resting orders, price levels and
// FIFO links of one instrument. It never matches; the engine applies the
// exchange rules and tells the book what rests, shrinks or leaves.

#include "book/active_bitmap.hpp"
#include "book/price_level.hpp"
#include "util/flat_hash_map.hpp"
#include "util/object_pool.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace exsim::book {

// Best ticks of an empty side, chosen so that "does tick t cross the opposite
// best" is a single signed comparison even when that side is empty.
inline constexpr std::int32_t kNoBidTick = -1;
inline constexpr std::int32_t kNoAskTick = std::numeric_limits<std::int32_t>::max();

// Order pool, level pool and OrderId map of one engine. Its instrument books
// all draw from it, so memory is bounded by the order limit, not by limit x
// books. Everything is allocated here; nothing grows afterwards.
struct OrderBookStorage {
    util::FlatHashMap<Order*> orders;
    util::ObjectPool<Order> order_pool;
    util::ObjectPool<PriceLevel> level_pool;

    explicit OrderBookStorage(std::size_t max_orders);
};

// Tick-indexed ladders with an occupancy bitmap per side. Rest, reduce and
// remove are O(1), plus an O(log64 ticks) bitmap search when the best level
// empties.
class OrderBook {
public:
    OrderBook(std::uint32_t tick_count, OrderBookStorage& storage);

    OrderBook(OrderBook&&) = default;
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // Appends an order to the FIFO of `tick`. Returns nullptr when the storage
    // is full or `id` already rests on a book of this storage.
    [[nodiscard]] Order* rest_order(OrderId id, std::int32_t tick, Quantity leaves, Side side,
                                    std::uint32_t context_index);
    // Takes `quantity` (at most the order's leaves) off the order, keeping its
    // FIFO position. An order reduced to zero stays until remove_order.
    void reduce_order(Order& order, Quantity quantity) noexcept;
    void remove_order(Order& order) noexcept;

    // The level at `tick` (0 .. tick_count - 1), or nullptr when it is empty.
    [[nodiscard]] const PriceLevel* price_level(Side side, std::int32_t tick) const noexcept {
        return (side == Side::BUY ? bid_ladder_ : ask_ladder_)[static_cast<std::size_t>(tick)];
    }
    // Sum of leaves at `tick`, 0 when the level is empty.
    [[nodiscard]] std::uint64_t level_volume(Side side, std::int32_t tick) const noexcept {
        const PriceLevel* level = price_level(side, tick);
        return level == nullptr ? 0 : level->total_volume;
    }
    // Nearest occupied tick at or after `from` in the given direction. `from`
    // may be one step outside the ladder, which finds nothing.
    [[nodiscard]] std::optional<std::int32_t> next_tick(Side side, std::int32_t from, bool ascending) const noexcept;

    // kNoBidTick / kNoAskTick when the side is empty.
    [[nodiscard]] std::int32_t best_bid_tick() const noexcept { return best_bid_tick_; }
    [[nodiscard]] std::int32_t best_ask_tick() const noexcept { return best_ask_tick_; }
    // Sum of leaves over the resting orders of one side.
    [[nodiscard]] std::uint64_t total_leaves(Side side) const noexcept {
        return leaves_by_side_[static_cast<std::size_t>(side)];
    }

private:
    OrderBookStorage& storage_;
    std::vector<PriceLevel*> bid_ladder_;
    std::vector<PriceLevel*> ask_ladder_;
    ActiveBitmap bid_active_levels_;
    ActiveBitmap ask_active_levels_;
    std::int32_t best_bid_tick_ = kNoBidTick;
    std::int32_t best_ask_tick_ = kNoAskTick;
    std::uint64_t leaves_by_side_[2] = {0, 0};  // BUY, SELL
};

}  // namespace exsim::book

#endif
