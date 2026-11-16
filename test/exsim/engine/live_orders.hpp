#ifndef TESTS_EXSIM_ENGINE_LIVE_ORDERS_HPP
#define TESTS_EXSIM_ENGINE_LIVE_ORDERS_HPP

#include <engine/matching_engine.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace exsim::testsupport {

// Test workloads assign monotonically increasing IDs. A priority-losing
// replace takes a new ID, so ascending IDs also describe expected FIFO order.
// This intentionally slow scan keeps inspection out of the production API.
inline std::vector<OrderInfo> live_orders(const MatchingEngine& engine) {
    std::vector<OrderInfo> orders;
    for (OrderId id = 1; orders.size() < engine.live_order_count(); ++id) {
        if (auto order = engine.find_order(id)) orders.push_back(*order);
    }
    return orders;
}

inline std::size_t level_orders(const MatchingEngine& engine, InstrumentId instrument,
                               Side side, Price price, OrderId* out, std::size_t capacity) {
    std::size_t seen = 0, written = 0;
    for (OrderId id = 1; seen < engine.live_order_count() && written < capacity; ++id) {
        if (auto order = engine.find_order(id)) {
            ++seen;
            if (out && order->instrument == instrument && order->side == side && order->price == price)
                out[written++] = id;
        }
    }
    return written;
}

// The feed builder retains its own FIFO inspection for packet assertions.
template <typename Book>
std::size_t level_orders(const Book& book, InstrumentId instrument, Side side, Price price,
                         OrderId* out, std::size_t capacity) {
    return book.level_orders(instrument, side, price, out, capacity);
}

}  // namespace exsim::testsupport

#endif
