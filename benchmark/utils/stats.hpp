#ifndef EXSIM_BENCH_STATS_HPP
#define EXSIM_BENCH_STATS_HPP

#include "utils/histogram.hpp"
#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

namespace bench {

// Per-operation summary of a run that timed batches of `batch_size` commands.
// Every sample in the histogram is one batch, so each percentile is a
// percentile of batch means: single-operation outliers are averaged with the
// rest of their batch.
struct Stats {
    std::uint64_t operations = 0;
    double mean_ns = 0;
    double p50_ns = 0;
    double p95_ns = 0;
    double p99_ns = 0;
    double p999_ns = 0;
    double p9999_ns = 0;
    double min_ns = 0;
    double max_ns = 0;
    // Commands divided by summed batch durations, including amortized clock
    // and loop overhead. Histogram updates and setup are outside those spans.
    double throughput_per_second = 0;

    static Stats from(const LatencyHistogram& batches, std::size_t batch_size) {
        Stats stats;
        if (batches.count() == 0) return stats;
        const double per_operation = 1.0 / static_cast<double>(batch_size);
        stats.operations = batches.count() * batch_size;
        stats.mean_ns = static_cast<double>(batches.sum()) / static_cast<double>(stats.operations);
        stats.p50_ns = static_cast<double>(batches.percentile(0.50)) * per_operation;
        stats.p95_ns = static_cast<double>(batches.percentile(0.95)) * per_operation;
        stats.p99_ns = static_cast<double>(batches.percentile(0.99)) * per_operation;
        stats.p999_ns = static_cast<double>(batches.percentile(0.999)) * per_operation;
        stats.p9999_ns = static_cast<double>(batches.percentile(0.9999)) * per_operation;
        stats.min_ns = static_cast<double>(batches.min()) * per_operation;
        stats.max_ns = static_cast<double>(batches.max()) * per_operation;
        if (batches.sum() > 0) {
            stats.throughput_per_second =
                static_cast<double>(stats.operations) * 1e9 / static_cast<double>(batches.sum());
        }
        return stats;
    }

    void report(benchmark::State& state) const {
        state.counters["Mean_ns"] = mean_ns;
        state.counters["P50_ns"] = p50_ns;
        state.counters["P95_ns"] = p95_ns;
        state.counters["P99_ns"] = p99_ns;
        state.counters["P99.9_ns"] = p999_ns;
        state.counters["P99.99_ns"] = p9999_ns;
        state.counters["Min_ns"] = min_ns;
        state.counters["Max_ns"] = max_ns;
        state.counters["Mops_per_s"] = throughput_per_second / 1e6;
    }
};

}  // namespace bench

#endif
