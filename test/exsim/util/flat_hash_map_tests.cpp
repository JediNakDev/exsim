#include "test/framework/test.hpp"
#include <util/flat_hash_map.hpp>

#include <random>
#include <unordered_map>

using exsim::util::FlatHashMap;

TEST_CASE(flat_hash_map, insert_find_erase) {
    FlatHashMap<int> map(8);
    CHECK(map.insert(1, 10));
    CHECK(map.insert(2, 20));
    CHECK(!map.insert(1, 99));  // duplicate
    CHECK(!map.insert(0, 1));   // 0 is the empty sentinel
    REQUIRE(map.find(1) != nullptr);
    CHECK_EQ(*map.find(1), 10);
    CHECK(map.erase(1));
    CHECK(!map.erase(1));
    CHECK(map.find(1) == nullptr);
    CHECK_EQ(*map.find(2), 20);
}

// FlatHashMap(4) gets the smallest table, 16 slots, and fills it to 50%: 8 keys.
TEST_CASE(flat_hash_map, full_map_rejects_until_a_key_is_erased) {
    FlatHashMap<int> map(4);
    for (std::uint64_t key = 1; key <= 8; ++key) {
        CHECK(map.insert(key, static_cast<int>(key)));
    }
    CHECK(!map.insert(9, 9));
    CHECK(map.erase(1));
    CHECK(map.insert(9, 9));
}

// Randomised differential test against std::unordered_map, with heavy
// erase churn to exercise backward-shift deletion across wrap-around.
TEST_CASE(flat_hash_map, matches_unordered_map_under_churn) {
    constexpr std::size_t kMaxEntries = 512;  // a power of two: the table holds exactly this many
    FlatHashMap<std::uint64_t> map(kMaxEntries);
    std::unordered_map<std::uint64_t, std::uint64_t> ref;
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<std::uint64_t> key_d(1, 2000);

    for (int step = 0; step < 200'000; ++step) {
        const std::uint64_t k = key_d(rng);
        switch (rng() % 3) {
            case 0: {
                const bool expect = ref.size() < kMaxEntries && !ref.count(k);
                const bool got = map.insert(k, k * 3);
                REQUIRE_EQ(got, expect);
                if (got) ref[k] = k * 3;
                break;
            }
            case 1: REQUIRE_EQ(map.erase(k), ref.erase(k) == 1); break;
            default: {
                const auto* v = map.find(k);
                const auto it = ref.find(k);
                REQUIRE_EQ(v != nullptr, it != ref.end());
                if (v) REQUIRE_EQ(*v, it->second);
                break;
            }
        }
    }
}
