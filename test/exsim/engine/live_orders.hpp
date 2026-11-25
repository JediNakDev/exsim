#ifndef TESTS_EXSIM_ENGINE_LIVE_ORDERS_HPP
#define TESTS_EXSIM_ENGINE_LIVE_ORDERS_HPP

#include <engine/matching_engine.hpp>
#include <test/framework/test.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <span>
#include <tuple>
#include <vector>

namespace exsim::testsupport {

// Test workloads assign monotonically increasing IDs. A priority-losing
// replace takes a new ID, so ascending IDs also describe expected FIFO order.
// This intentionally slow scan keeps inspection out of the production API.
// Readers use order_id, so a lookup must return the ID it asked for.
inline std::vector<OrderInfo> live_orders(const MatchingEngine& engine) {
    std::vector<OrderInfo> orders;
    for (OrderId id = 1; orders.size() < engine.live_order_count(); ++id) {
        if (auto order = engine.find_order(id)) {
            CHECK_EQ(order->order_id, id);
            orders.push_back(*order);
        }
    }
    return orders;
}

inline std::size_t level_orders(const MatchingEngine& engine, InstrumentId instrument, Side side, Price price,
                                OrderId* out, std::size_t capacity) {
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

// A reader that visits every level groups one live_orders() result here
// instead of calling level_orders() per level, which rescans every ID each
// time. IDs within a level stay ascending.
class LevelOrders {
public:
    explicit LevelOrders(std::span<const OrderInfo> orders) {
        for (const OrderInfo& order : orders)
            levels_[{order.instrument, order.side, order.price}].push_back(order.order_id);
    }

    std::size_t level_orders(InstrumentId instrument, Side side, Price price, OrderId* out,
                             std::size_t capacity) const {
        const auto level = levels_.find({instrument, side, price});
        if (level == levels_.end()) return 0;
        const std::size_t written = std::min(capacity, level->second.size());
        std::copy_n(level->second.begin(), written, out);
        return written;
    }

private:
    std::map<std::tuple<InstrumentId, Side, Price>, std::vector<OrderId>> levels_;
};

}  // namespace exsim::testsupport

#endif
