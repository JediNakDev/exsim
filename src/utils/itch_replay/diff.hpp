#ifndef EXSIM_ITCH_REPLAY_DIFF_HPP
#define EXSIM_ITCH_REPLAY_DIFF_HPP

// Depth diff between the matching engine and ReferenceBook. `ok` is true
// when the top `depth` levels of both sides agree on price, total quantity
// and order count.

#include "reference_book.hpp"
#include <engine/matching_engine.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace exsim::itch_replay {

struct DiffResult {
    bool ok = true;
    std::string detail;
};

inline DiffResult diff_books(const MatchingEngine& engine, InstrumentId instrument, const ReferenceBook& ref,
                             std::size_t depth = 10) {
    const auto reference = ref.top(depth);
    std::vector<LevelInfo> engine_side(depth);
    std::ostringstream err;

    auto cmp = [&](const char* name, Side side, const std::vector<ReferenceBook::Level>& ref_side) {
        const std::size_t engine_levels = engine.depth(instrument, side, engine_side.data(), depth);
        if (engine_levels != ref_side.size()) {
            err << name << " level count: engine=" << engine_levels << " ref=" << ref_side.size() << "; ";
            return false;
        }
        for (std::size_t i = 0; i < engine_levels; ++i) {
            if (engine_side[i].price != ref_side[i].price || engine_side[i].qty != ref_side[i].total_qty ||
                engine_side[i].order_count != ref_side[i].order_count) {
                err << name << "[" << i << "] mismatch: " << "engine(p=" << engine_side[i].price
                    << " q=" << engine_side[i].qty << " n=" << engine_side[i].order_count << ") "
                    << "ref(p=" << ref_side[i].price << " q=" << ref_side[i].total_qty
                    << " n=" << ref_side[i].order_count << "); ";
                return false;
            }
        }
        return true;
    };

    bool ok_b = cmp("bid", Side::BUY, reference.bids);
    bool ok_a = cmp("ask", Side::SELL, reference.asks);
    DiffResult out;
    out.ok = ok_b && ok_a;
    if (!out.ok) out.detail = err.str();
    return out;
}

}  // namespace exsim::itch_replay

#endif
