#include "utils/runner.hpp"

using namespace bench;

// Each aggressive order is larger than a resting order, so it fills two or
// three of them and then completes. Buys and sells alternate against a book
// of 64k resting orders of quantity 10 per side.
static void BM_SweepWithFills(benchmark::State& state) {
    run_commands(state, "SweepWithFills", [](Scenario& scenario) {
        constexpr exsim::Quantity kRestingQuantity = 10;
        constexpr exsim::Quantity kAggressorQuantity = 25;
        for (int level = 1; level <= kLevelsPerSide; ++level) {
            for (int i = 0; i < 128; ++i) {
                scenario.apply(scenario.new_order(scenario.next_cl_ord_id(), exsim::Side::BUY,
                                                  price_at(exsim::Side::BUY, level), kRestingQuantity));
                scenario.apply(scenario.new_order(scenario.next_cl_ord_id(), exsim::Side::SELL,
                                                  price_at(exsim::Side::SELL, level), kRestingQuantity));
            }
        }
        std::vector<exsim::Command> commands;
        for (std::size_t i = 0; i < 32768; ++i) {
            const exsim::Side side = i % 2 == 0 ? exsim::Side::BUY : exsim::Side::SELL;
            const exsim::Side opposite = side == exsim::Side::BUY ? exsim::Side::SELL : exsim::Side::BUY;
            commands.push_back(scenario.new_order(scenario.next_cl_ord_id(), side,
                                                  price_at(opposite, kLevelsPerSide), kAggressorQuantity));
        }
        return commands;
    });
}

BENCHMARK(BM_SweepWithFills)->Iterations(kIterations)->Unit(benchmark::kMillisecond);
