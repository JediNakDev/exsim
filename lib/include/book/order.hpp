#ifndef EXSIM_BOOK_ORDER_HPP
#define EXSIM_BOOK_ORDER_HPP

#include "common/types.hpp"
#include <cstdint>

namespace exsim::book {

class PriceLevel;

// One cache line per resting order: a fill touches a single line.
struct alignas(64) Order {
    OrderId id;
    Order* prev_order;
    Order* next_order;
    PriceLevel* parent_level;
    std::int32_t tick;  // price as a ladder index
    Quantity remaining_quantity;
    // Owner metadata slot; the book never interprets it.
    std::uint32_t context_index;
    Side side;
};

static_assert(sizeof(Order) == 64, "Order must stay exactly one cache line");

}  // namespace exsim::book

#endif
