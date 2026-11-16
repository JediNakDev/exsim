#include "utils/runner.hpp"

using namespace bench;

// Replaces random orders of a 64k-order book with a new token, price and
// quantity. Each replace loses time priority.
static void BM_ReplaceOrder(benchmark::State& state) {
    run_commands(state, "ReplaceOrder", [](Scenario& scenario) {
        std::vector<RestingOrder> resting = scenario.rest_random_orders(65536);
        std::vector<exsim::Command> commands;
        for (std::size_t i = 0; i < 65536; ++i) {
            RestingOrder& order = resting[scenario.rng()() % resting.size()];
            const exsim::ClOrdId new_cl_ord_id = scenario.next_cl_ord_id();
            commands.push_back(Scenario::replace(order.cl_ord_id, new_cl_ord_id, scenario.random_price(order.side),
                                                 scenario.random_quantity()));
            order.cl_ord_id = new_cl_ord_id;
        }
        return commands;
    });
}

BENCHMARK(BM_ReplaceOrder)->Iterations(kIterations)->Unit(benchmark::kMillisecond);
