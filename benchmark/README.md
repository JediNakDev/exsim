# Benchmarks

Install Google Benchmark (`brew install google-benchmark` on macOS,
`sudo apt-get install libbenchmark-dev` on Ubuntu), then run:

```sh
make -j4 bench
make bench FILTER=MixedWorkload ARGS="--csv=output/mixed.csv --core=0"
make -j4 bench-build
```

`FILTER` is a Google Benchmark regular expression. `ARGS` passes runner options.
The binary and default CSV live in `build/bench/`. `BENCH_BUILD` selects another
build directory, including when `BUILD` is set for tests on another platform.
Create custom CSV directories first. Each run replaces its CSV.

## Workloads

All six cases call `MatchingEngine::handle(Command, EventBuffer)`, as the
exchange core does. They measure resting new orders, cancels, priority-losing
replaces, multi-fill sweeps, a 50/35/15 new/cancel/replace mix, and a 5/93/2
cancel-heavy mix. Fixed-seed scripts use one continuous-phase instrument and
an explicit 131,072-order capacity. Each case creates a fresh engine and
prepares its script before timing. The first of ten iterations warms caches
and checks every command for rejection; nine iterations feed the CSV.

No standalone matcher, benchmark-only sharding, gateway, socket, market-data
encoding or management API work is measured. Event production and buffer
clearing are included. Event consumption is not measured.

## Timing

Two `steady_clock` reads bracket 32 commands. The runner prints its minimum
observed positive clock delta at startup. A fixed log-linear histogram records
batch nanoseconds in about 15 KiB, with 32 buckets per power of two. CSV
percentiles divide bucket midpoints by 32, so they are **percentiles of batch
means**, not individual-command tail latency. Buckets have about 3% relative
width. Min, max and mean use exact recorded durations.

Throughput is commands divided by summed batch durations. It includes amortized
clock reads, command-loop bookkeeping and optimization barriers. Histogram
updates, script generation, warmup, engine construction and destruction are
excluded. No guessed clock-overhead subtraction is applied. The Google
Benchmark time column also includes harness overhead and warmup; use the custom
CSV and `Mean_ns`, `P50_ns`, `P99_ns`, `Mops_per_s` counters for comparisons.
Fixed iterations override `--benchmark_min_time`; use repetitions for profiling.

Bench uses the existing perf flags: C++23, `-O3 -march=native -DNDEBUG -flto
-fno-omit-frame-pointer`. Release uses `-O2 -g` with assertions enabled, so bench
is explicitly a perf build, not the default release build. There is no
benchmark-only engine or pool define. Pinning is attempted; macOS supplies only
an affinity hint, not a guaranteed core assignment.

## Replay and profiling

```sh
make -j4 itch-replay
./build/perf/itch_replay --synth 100000 --validate
```

Replay remains a separate standalone order-book tool, not part of this suite.
See [profiling](profiling/README.md) and [current results](../results/README.md).
