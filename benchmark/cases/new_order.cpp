#include "utils/runner.hpp"

using namespace bench;

// Resting limit orders added to a book that already holds 32k orders.
static void BM_NewOrder(benchmark::State& state) {
    run_commands(state, "NewOrder", [](Scenario& scenario) {
        scenario.rest_random_orders(32768);
        std::vector<exsim::Command> commands;
        for (std::size_t i = 0; i < 65536; ++i) {
            const exsim::Side side = scenario.random_side();
            commands.push_back(scenario.new_order(scenario.next_cl_ord_id(), side,
                                                  scenario.random_price(side),
                                                  scenario.random_quantity()));
        }
        return commands;
    });
}

BENCHMARK(BM_NewOrder)->Iterations(kIterations)->Unit(benchmark::kMillisecond);
