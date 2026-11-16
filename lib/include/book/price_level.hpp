#ifndef EXSIM_BOOK_PRICE_LEVEL_HPP
#define EXSIM_BOOK_PRICE_LEVEL_HPP

#include "book/order.hpp"
#include <cstdint>

namespace exsim::book {

// One price of one side: the FIFO of its resting orders (a doubly linked
// list, oldest first) and their aggregates. Append and unlink are O(1).
class PriceLevel {
public:
    std::int32_t tick;
    std::uint32_t order_count = 0;
    // Sum of the orders' leaves. 64 bits: a level can hold more than one
    // order of the maximum 32-bit quantity.
    std::uint64_t total_volume = 0;
    Order* head_order = nullptr;
    Order* tail_order = nullptr;

    explicit PriceLevel(std::int32_t tick_) noexcept : tick(tick_) {}

    void add_order(Order* order) noexcept {
        order->parent_level = this;
        order->prev_order = tail_order;
        order->next_order = nullptr;
        if (tail_order) {
            tail_order->next_order = order;
        } else {
            head_order = order;
        }
        tail_order = order;
        total_volume += order->remaining_quantity;
        ++order_count;
    }

    void remove_order(Order* order) noexcept {
        if (order->prev_order) {
            order->prev_order->next_order = order->next_order;
        } else {
            head_order = order->next_order;
        }
        if (order->next_order) {
            order->next_order->prev_order = order->prev_order;
        } else {
            tail_order = order->prev_order;
        }
        total_volume -= order->remaining_quantity;
        --order_count;
    }
};

}  // namespace exsim::book

#endif
