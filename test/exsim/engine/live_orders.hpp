#ifndef TESTS_EXSIM_ENGINE_LIVE_ORDERS_HPP
#define TESTS_EXSIM_ENGINE_LIVE_ORDERS_HPP

#include <engine/matching_engine.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace exsim::testsupport {

// Every live order of every instrument in ascending OrderId order, collected
// by walking each price level through the public queries. Test-only: the
// engine itself keeps no engine-wide order list.
inline std::vector<OrderInfo> live_orders(const MatchingEngine& engine) {
    std::vector<OrderInfo> orders;
    std::vector<LevelInfo> levels(engine.live_order_count());
    std::vector<OrderId> level_order_ids(engine.live_order_count());
    for (std::size_t instrument_index = 1; instrument_index <= engine.instrument_count(); ++instrument_index) {
        const InstrumentId instrument = static_cast<InstrumentId>(instrument_index);
        for (const Side side : {Side::BUY, Side::SELL}) {
            const std::size_t level_count = engine.depth(instrument, side, levels.data(), levels.size());
            for (std::size_t level = 0; level < level_count; ++level) {
                const std::size_t order_count = engine.level_orders(instrument, side, levels[level].price,
                                                                    level_order_ids.data(), level_order_ids.size());
                for (std::size_t i = 0; i < order_count; ++i) {
                    orders.push_back(engine.find_order(level_order_ids[i]).value());
                }
            }
        }
    }
    std::ranges::sort(orders, {}, &OrderInfo::order_id);
    return orders;
}

}  // namespace exsim::testsupport

#endif
