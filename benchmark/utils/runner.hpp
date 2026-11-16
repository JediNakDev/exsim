#ifndef EXSIM_BENCH_RUNNER_HPP
#define EXSIM_BENCH_RUNNER_HPP

// Time batches to amortize clock reads. Percentiles describe batch means,
// not individual command tails. Setup and warmup are excluded from the CSV.

#include "utils/csv_writer.hpp"
#include "utils/histogram.hpp"
#include "utils/scenario.hpp"
#include "utils/stats.hpp"
#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bench {

inline constexpr std::size_t kBatchSize = 32;
// The first iteration warms caches and pages and is not recorded.
inline constexpr int kWarmupIterations = 1;
inline constexpr int kIterations = 10;

// `prepare` builds the book through Scenario::apply (untimed) and returns the
// commands to time.
template <typename Prepare>
void run_commands(benchmark::State& state, const std::string& name, Prepare&& prepare) {
    LatencyHistogram batches;
    std::uint64_t commands_run = 0;
    std::uint64_t events_emitted = 0;
    std::uint64_t not_applied = 0;
    std::size_t live_orders_after = 0;
    int iteration = 0;

    for (auto _ : state) {
        state.PauseTiming();
        auto scenario = std::make_unique<Scenario>();
        const std::vector<exsim::Command> commands = prepare(*scenario);
        if (commands.empty() || commands.size() % kBatchSize != 0) {
            state.SkipWithError("command count must be a nonzero multiple of batch size");
            break;
        }
        const bool recorded = iteration++ >= kWarmupIterations;
        auto& engine = scenario->engine();
        auto& events = scenario->events();
        state.ResumeTiming();

        for (std::size_t first = 0; first + kBatchSize <= commands.size(); first += kBatchSize) {
            const auto start = std::chrono::steady_clock::now();
            for (std::size_t i = first; i < first + kBatchSize; ++i) {
                events.clear();
                auto result = engine.handle(commands[i], events);
                benchmark::DoNotOptimize(result);
                not_applied += result != exsim::HandleResult::Applied;
                events_emitted += events.size();
                // Validate the deterministic script during warmup only.
                if (!recorded) {
                    for (const auto& event : events) {
                        if (event.type == exsim::EventType::Rejected)
                            throw std::runtime_error("benchmark command rejected");
                    }
                }
            }
            commands_run += kBatchSize;
            const auto stop = std::chrono::steady_clock::now();
            benchmark::ClobberMemory();
            if (recorded) {
                batches.record(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()));
            }
        }
        live_orders_after = engine.live_order_count();
        state.PauseTiming();
        scenario.reset();
        state.ResumeTiming();
    }

    if (not_applied != 0) {
        state.SkipWithError("engine returned InsufficientOutput");
        return;
    }
    const Stats stats = Stats::from(batches, kBatchSize);
    stats.report(state);
    state.counters["EventsPerCommand"] =
        static_cast<double>(events_emitted) / static_cast<double>(commands_run);
    state.counters["LiveOrdersAfter"] = static_cast<double>(live_orders_after);
    state.SetItemsProcessed(static_cast<std::int64_t>(commands_run));
    if (csv()) csv()->write(name, stats);
}

}  // namespace bench

#endif
