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

// Creates, destroys and re-creates objects through every node. Run under
// ASan and UBSan (MODE=debug) it also checks the node's lifetime handling.
TEST_CASE(object_pool, recreated_objects_keep_their_own_values) {
    struct alignas(64) Record {  // over-aligned like book::Order
        std::uint64_t id;
        std::uint32_t round;
    };
    constexpr std::uint64_t kCapacity = 8;
    ObjectPool<Record> pool(kCapacity);
    Record* records[kCapacity] = {};
    for (std::uint32_t round = 0; round < 3; ++round) {
        for (std::uint64_t i = 0; i < kCapacity; ++i) {
            records[i] = pool.create(Record{i, round});
            REQUIRE(records[i] != nullptr);
            CHECK_EQ(reinterpret_cast<std::uintptr_t>(records[i]) % alignof(Record), 0u);
        }
        CHECK(pool.create(Record{kCapacity, round}) == nullptr);
        for (std::uint64_t i = 0; i < kCapacity; ++i) {
            CHECK_EQ(records[i]->id, i);
            CHECK_EQ(records[i]->round, round);
        }
        // Freeing a node writes its link into that node only.
        for (std::uint64_t i = 0; i < kCapacity; i += 2) pool.destroy(records[i]);
        for (std::uint64_t i = 1; i < kCapacity; i += 2) {
            CHECK_EQ(records[i]->id, i);
            CHECK_EQ(records[i]->round, round);
        }
        for (std::uint64_t i = 1; i < kCapacity; i += 2) pool.destroy(records[i]);
    }
}
