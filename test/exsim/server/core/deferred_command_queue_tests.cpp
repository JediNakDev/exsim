// DeferredCommandQueue: the capacity is rounded up to a power of two and
// positions wrap with a mask, keeping FIFO order across many wraps.

#include "server/core/deferred_command_queue.hpp"
#include "test/framework/test.hpp"

#include <bit>
#include <cstdint>

using exsim::Command;
using exsim::server::DeferredCommandQueue;

TEST_CASE(deferred_command_queue, capacity_is_the_next_power_of_two) {
    CHECK_EQ(DeferredCommandQueue(0).capacity(), 1u);
    CHECK_EQ(DeferredCommandQueue(1).capacity(), 1u);
    CHECK_EQ(DeferredCommandQueue(3).capacity(), 4u);
    CHECK_EQ(DeferredCommandQueue(8).capacity(), 8u);
    CHECK_EQ(DeferredCommandQueue(9).capacity(), 16u);
}

TEST_CASE(deferred_command_queue, fifo_order_survives_wrap_around) {
    DeferredCommandQueue queue(3);
    REQUIRE_EQ(queue.capacity(), 4u);
    Command command;
    std::uint64_t next_pushed = 1;
    std::uint64_t next_popped = 1;
    for (int round = 0; round < 10; ++round) {
        while (queue.size() < queue.capacity()) {
            command.timestamp = next_pushed++;
            REQUIRE(queue.push(command));
        }
        command.timestamp = 0;
        CHECK(!queue.push(command));  // full
        for (int popped = 0; popped < 3; ++popped) {
            REQUIRE(queue.pop(command));
            CHECK_EQ(command.timestamp, next_popped++);
        }
    }
    while (queue.pop(command)) CHECK_EQ(command.timestamp, next_popped++);
    CHECK_EQ(next_popped, next_pushed);
    CHECK_EQ(queue.size(), 0u);
}
