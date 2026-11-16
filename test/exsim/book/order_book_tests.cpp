// exsim::book::OrderBook as the matching engine uses it: passive storage that
// never matches. Matching, capacity and price rules are covered by the engine
// tests, which drive MatchingEngine::handle.

#include "test/framework/test.hpp"
#include <book/order_book.hpp>
#include <test/framework/alloc_counter.hpp>

using namespace exsim;
using namespace exsim::book;

TEST_CASE(order_book, resting_keeps_fifo_and_never_matches_a_crossed_book) {
    OrderBookStorage storage(3);
    OrderBook book(101, storage);
    CHECK_EQ(book.best_bid_tick(), kNoBidTick);
    CHECK_EQ(book.best_ask_tick(), kNoAskTick);

    Order* const first = book.rest_order(41, 60, 15, Side::BUY, 7);
    REQUIRE(first != nullptr);
    REQUIRE(book.rest_order(42, 60, 10, Side::BUY, 8) != nullptr);
    REQUIRE(book.rest_order(43, 50, 8, Side::SELL, 9) != nullptr);
    // A crossed (auction) book stays as it was rested.
    CHECK_EQ(book.best_bid_tick(), 60);
    CHECK_EQ(book.best_ask_tick(), 50);
    const PriceLevel* const level = book.price_level(Side::BUY, 60);
    REQUIRE(level != nullptr);
    CHECK_EQ(level->total_volume, 25u);
    CHECK_EQ(level->order_count, 2u);
    CHECK_EQ(level->head_order->id, 41u);
    CHECK_EQ(level->head_order->next_order->id, 42u);
    CHECK_EQ(first->context_index, 7u);
    CHECK(book.price_level(Side::SELL, 60) == nullptr);
    CHECK_EQ(book.total_leaves(Side::BUY), 25u);
    CHECK_EQ(book.total_leaves(Side::SELL), 8u);
}

TEST_CASE(order_book, reduce_and_remove_update_level_side_totals_and_best_ticks) {
    OrderBookStorage storage(4);
    OrderBook book(101, storage);
    Order* const best_bid = book.rest_order(1, 60, 15, Side::BUY, 0);
    Order* const second_bid = book.rest_order(2, 60, 10, Side::BUY, 0);
    Order* const low_bid = book.rest_order(3, 0, 4, Side::BUY, 0);
    Order* const ask = book.rest_order(4, 100, 9, Side::SELL, 0);
    REQUIRE(best_bid != nullptr && second_bid != nullptr && low_bid != nullptr && ask != nullptr);

    book.reduce_order(*best_bid, 15);  // zero leaves stay until removed
    CHECK_EQ(best_bid->remaining_quantity, 0u);
    CHECK_EQ(book.price_level(Side::BUY, 60)->total_volume, 10u);
    CHECK_EQ(book.price_level(Side::BUY, 60)->order_count, 2u);
    CHECK_EQ(book.total_leaves(Side::BUY), 14u);

    book.remove_order(*best_bid);
    CHECK_EQ(book.price_level(Side::BUY, 60)->head_order->id, 2u);
    CHECK_EQ(book.best_bid_tick(), 60);
    book.remove_order(*second_bid);  // the level empties: the best moves to the far edge
    CHECK(book.price_level(Side::BUY, 60) == nullptr);
    CHECK_EQ(book.best_bid_tick(), 0);
    book.remove_order(*low_bid);
    CHECK_EQ(book.best_bid_tick(), kNoBidTick);
    CHECK_EQ(book.total_leaves(Side::BUY), 0u);
    book.remove_order(*ask);
    CHECK_EQ(book.best_ask_tick(), kNoAskTick);
    CHECK_EQ(book.total_leaves(Side::SELL), 0u);
}

TEST_CASE(order_book, next_tick_reaches_both_ladder_edges_on_both_sides) {
    OrderBookStorage storage(4);
    OrderBook book(1001, storage);
    Order* const low_bid = book.rest_order(1, 0, 1, Side::BUY, 0);
    REQUIRE(low_bid != nullptr);
    REQUIRE(book.rest_order(2, 1000, 1, Side::BUY, 0) != nullptr);
    REQUIRE(book.rest_order(3, 0, 1, Side::SELL, 0) != nullptr);
    Order* const high_ask = book.rest_order(4, 1000, 1, Side::SELL, 0);
    REQUIRE(high_ask != nullptr);
    for (const Side side : {Side::BUY, Side::SELL}) {
        CHECK_EQ(book.next_tick(side, 0, true).value_or(-1), 0);
        CHECK_EQ(book.next_tick(side, 1, true).value_or(-1), 1000);
        CHECK_EQ(book.next_tick(side, 1000, false).value_or(-1), 1000);
        CHECK_EQ(book.next_tick(side, 999, false).value_or(-1), 0);
        CHECK(!book.next_tick(side, 1001, true).has_value());  // one step past either edge
        CHECK(!book.next_tick(side, -1, false).has_value());
    }
    book.remove_order(*low_bid);
    book.remove_order(*high_ask);
    CHECK(!book.next_tick(Side::BUY, 999, false).has_value());
    CHECK(!book.next_tick(Side::SELL, 1, true).has_value());
}

TEST_CASE(order_book, books_on_one_storage_share_capacity_and_order_ids_but_not_levels) {
    OrderBookStorage storage(3);
    OrderBook first_book(201, storage);
    OrderBook second_book(201, storage);
    Order* const first_bid = first_book.rest_order(1, 100, 5, Side::BUY, 0);
    REQUIRE(first_bid != nullptr);
    // rest_order has no lookup of its own: the map insert refuses a resting id.
    CHECK(first_book.rest_order(1, 99, 1, Side::BUY, 3) == nullptr);
    CHECK(second_book.rest_order(1, 99, 1, Side::BUY, 3) == nullptr);
    CHECK(first_book.price_level(Side::BUY, 99) == nullptr);
    CHECK(second_book.price_level(Side::BUY, 99) == nullptr);
    CHECK_EQ(first_book.total_leaves(Side::BUY), 5u);

    REQUIRE(second_book.rest_order(2, 100, 7, Side::SELL, 1) != nullptr);
    REQUIRE(second_book.rest_order(3, 101, 7, Side::SELL, 2) != nullptr);
    CHECK(second_book.rest_order(4, 102, 1, Side::SELL, 3) == nullptr);  // the storage is full
    CHECK(first_book.rest_order(4, 50, 1, Side::BUY, 3) == nullptr);
    CHECK(first_book.price_level(Side::SELL, 100) == nullptr);
    CHECK_EQ(first_book.best_ask_tick(), kNoAskTick);
    CHECK_EQ(second_book.best_bid_tick(), kNoBidTick);
    CHECK_EQ(second_book.total_leaves(Side::SELL), 14u);

    // A slot freed by one book is usable by the other, and so is its id.
    first_book.remove_order(*first_bid);
    CHECK_EQ(first_book.best_bid_tick(), kNoBidTick);
    CHECK(second_book.rest_order(1, 102, 1, Side::SELL, 3) != nullptr);
    CHECK_EQ(second_book.best_ask_tick(), 100);
}

TEST_CASE(order_book, nothing_allocates_after_construction) {
    if (!testfw::allocation_counting_available()) return;
    OrderBookStorage storage(2);
    OrderBook book(101, storage);
    std::size_t count;
    {
        testfw::AllocationScope allocations;
        Order* const bid = book.rest_order(1, 10, 10, Side::BUY, 1);
        Order* const ask = book.rest_order(2, 20, 10, Side::SELL, 2);
        const bool both_rested = bid != nullptr && ask != nullptr;
        if (both_rested) {
            book.reduce_order(*bid, 3);
            book.remove_order(*ask);
            book.remove_order(*bid);
        }
        const bool slots_reused = both_rested && book.rest_order(3, 30, 5, Side::SELL, 3) != nullptr &&
                                  book.rest_order(4, 5, 5, Side::BUY, 4) != nullptr;
        count = allocations.count();
        CHECK(slots_reused);
    }
    CHECK_EQ(count, 0u);
}
