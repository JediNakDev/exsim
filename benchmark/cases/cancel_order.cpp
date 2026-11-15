#include "utils/runner.hpp"

#include <algorithm>

using namespace bench;

// Cancels every order of a 64k-order book in random order.
static void BM_CancelOrder(benchmark::State& state) {
    run_commands(state, "CancelOrder", [](Scenario& scenario) {
        std::vector<RestingOrder> resting = scenario.rest_random_orders(65536);
        std::shuffle(resting.begin(), resting.end(), scenario.rng());
        std::vector<exsim::Command> commands;
        for (const RestingOrder& order : resting) commands.push_back(Scenario::cancel(order.cl_ord_id));
        return commands;
    });
}

BENCHMARK(BM_CancelOrder)->Iterations(kIterations)->Unit(benchmark::kMillisecond);
