// exsim::util::ObjectPool: fixed capacity, bounded exhaustion and slot reuse.

#include "test/framework/test.hpp"
#include <util/object_pool.hpp>
#include <test/framework/alloc_counter.hpp>

#include <cstdint>

using exsim::util::ObjectPool;

TEST_CASE(object_pool, pool_holds_exactly_its_capacity_and_reuses_slots) {
    ObjectPool<std::uint64_t> pool(5);

    std::uint64_t* objects[5] = {};
    std::size_t allocations = 0;
    std::uint64_t* overflow = nullptr;
    std::uint64_t* reused = nullptr;
    {
        testfw::AllocationScope scope;
        for (std::uint64_t i = 0; i < 5; ++i) {
            objects[i] = pool.create(i);
        }
        overflow = pool.create(99);
        pool.destroy(objects[2]);
        reused = pool.create(7);
        allocations = scope.count();
    }
    for (std::uint64_t i = 0; i < 5; ++i) {
        REQUIRE(objects[i] != nullptr);
    }
    // Nodes come out in address order.
    CHECK(objects[1] > objects[0]);
    CHECK(objects[4] > objects[3]);
    CHECK(overflow == nullptr);
    CHECK(reused == objects[2]);
    CHECK_EQ(*reused, 7u);
    CHECK_EQ(*objects[4], 4u);
    CHECK_EQ(allocations, 0u);
}

TEST_CASE(object_pool, empty_pool_creates_nothing) {
    ObjectPool<std::uint64_t> pool(0);
    CHECK(pool.create(1) == nullptr);
}

TEST_CASE(object_pool, throwing_constructor_restores_the_free_slot) {
    struct SometimesThrows {
        int value;
        explicit SometimesThrows(bool fail) : value(42) { if (fail) throw 7; }
    };
    ObjectPool<SometimesThrows> pool(1);
    bool threw = false;
    try { (void)pool.create(true); } catch (int) { threw = true; }
    REQUIRE(threw);
    auto* value = pool.create(false);
    REQUIRE(value != nullptr);
    CHECK_EQ(value->value, 42);
    CHECK(pool.create(false) == nullptr);
    pool.destroy(value);
}
