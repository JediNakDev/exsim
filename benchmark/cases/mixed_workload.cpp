#include "utils/runner.hpp"

using namespace bench;

namespace {

struct CommandMix {
    unsigned new_order_percent;
    unsigned cancel_percent;  // the rest are replaces
};

// New orders rest and never cross, so the live-order model below stays exact
// and every cancel and replace targets an order that is on the book.
std::vector<exsim::Command> mixed_commands(Scenario& scenario, std::size_t book_size, std::size_t command_count,
                                           CommandMix mix) {
    std::vector<RestingOrder> live = scenario.rest_random_orders(book_size);
    std::vector<exsim::Command> commands;
    commands.reserve(command_count);
    for (std::size_t i = 0; i < command_count; ++i) {
        const unsigned roll = static_cast<unsigned>(scenario.rng()() % 100);
        if (live.empty() || roll < mix.new_order_percent) {
            const RestingOrder order{scenario.next_cl_ord_id(), scenario.random_side()};
            commands.push_back(scenario.new_order(order.cl_ord_id, order.side, scenario.random_price(order.side),
                                                  scenario.random_quantity()));
            live.push_back(order);
            continue;
        }
        const std::size_t target = scenario.rng()() % live.size();
        if (roll < mix.new_order_percent + mix.cancel_percent) {
            commands.push_back(Scenario::cancel(live[target].cl_ord_id));
            live[target] = live.back();
            live.pop_back();
        } else {
            const exsim::ClOrdId new_cl_ord_id = scenario.next_cl_ord_id();
            commands.push_back(Scenario::replace(live[target].cl_ord_id, new_cl_ord_id,
                                                 scenario.random_price(live[target].side), scenario.random_quantity()));
            live[target].cl_ord_id = new_cl_ord_id;
        }
    }
    return commands;
}

}  // namespace

// 50% new order, 35% cancel, 15% replace on a 16k-order book.
static void BM_MixedWorkload(benchmark::State& state) {
    run_commands(state, "MixedWorkload", [](Scenario& scenario) {
        return mixed_commands(scenario, 16384, 65536, {.new_order_percent = 50, .cancel_percent = 35});
    });
}

// Cancel-dominated traffic: 93% cancel, 5% new order, 2% replace.
static void BM_CancelHeavyWorkload(benchmark::State& state) {
    run_commands(state, "CancelHeavyWorkload", [](Scenario& scenario) {
        return mixed_commands(scenario, 65536, 65536, {.new_order_percent = 5, .cancel_percent = 93});
    });
}

BENCHMARK(BM_MixedWorkload)->Iterations(kIterations)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_CancelHeavyWorkload)->Iterations(kIterations)->Unit(benchmark::kMillisecond);
