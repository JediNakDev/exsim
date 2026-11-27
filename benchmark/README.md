# Benchmarks

`make bench` times the matching engine alone. `make bench-wire` times orders
through a running exchange as its client sees them, to the OUCH ack and to the
ITCH feed message; see [Wire latency](#wire-latency).

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
Give it a folder that holds only bench builds: make does not rebuild an object
when only the flags change, so a release folder would give release numbers.
Create custom CSV directories first. Each run replaces its CSV.

## Workloads

All six cases call `MatchingEngine::handle(Command, EventBuffer)`, as the
exchange core does. They measure resting new orders, cancels, priority-losing
replaces, multi-fill sweeps, a 50/35/15 new/cancel/replace mix, and a 5/93/2
cancel-heavy mix. Fixed-seed scripts use one continuous-phase instrument and
an explicit 131,072-order capacity. Each case creates a fresh engine and
prepares its script before timing. The first of ten iterations warms caches
and checks every command for rejection; nine iterations feed the CSV.

No standalone matcher, benchmark-only sharding, gateway, socket or market-data
encoding work is measured here. `bench-wire` measures the running exchange's
sockets, gateway, matching and market data together. Event production
and buffer clearing are included. Event consumption is not measured.

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
benchmark-only engine or pool define. `--core` tries to pin on Linux only; when
the pin fails, and always on macOS, which has no core pinning, the run reports
that pinning is not available.

## Wire latency

```sh
make -j4 bench-wire
make bench-wire ARGS="--orders=100000 --window=64 --core=2 --csv=output/wire.csv"
```

`bench-wire` builds `exsim_exchanged` and `exsim_wire_bench` with the bench
flags into `BENCH_BUILD` and runs the client. It needs no Google Benchmark. Run
the client by its path, as `make bench-wire` does: it works in the folder of
that path, without following symlinks, and a bare name found on `PATH` is a
usage error. There it writes `wire_latency.conf` (one instrument in continuous
trading, no journal, no admin socket) and starts `exsim_exchanged --config` as
a child. It logs in one OUCH 4.2 user over loopback TCP and reads the ITCH 5.0
feed as MoldUDP64 unicast to 127.0.0.1. The report's last lines name the
exchange it started and the CSV. There is no benchmark-only code in `src/`.

Each workload runs once with one order in flight and, unless `--window=1`,
once with `--window` (default 64) in flight, `--orders` (default 50,000, at most
10^9) timed orders per pass, all on one exchange and one OUCH session:

- `rest`: buys at $100.00 that rest. The feed message is the ITCH Add Order.
- `cross`: untimed sells at $150.00 rest first, then each timed buy at $150.00
  fills one fully. The feed message is the ITCH Order Executed for that sell.

Both numbers are elapsed time on the client, from before it encodes the OUCH
Enter Order:

- `ack`: to after it decodes the Order Accepted and checks its token.
- `feed`: to after it decodes the order's feed message and, for `cross`, checks
  that it is a full fill.

They include the client's encode, send, receive and decode and everything else
it does in between (with a window, other orders' sends and replies), the
loopback path of the order and of that reply, and whatever the exchange does
until that reply reaches the client. The client spins. The exchange runs with
its default options, without busy polling, so a sample can include its wake-up
from a 1 ms poll. `feed` can be lower or higher than `ack`. They exclude network
cards, switches and multicast, the journal, recovery, process start and login,
and the untimed resting sells. For what happens inside a sample, read
`WireClient::run` in `benchmark/wire/main.cpp` and `ServerRuntime::poll_once` in
`src/server/runtime/server_runtime.hpp`.

A pass's rate is its orders divided by the time from before its first order is
encoded to the end of the client loop iteration that completes the pass: the
completion rate of the client and the exchange together, not the exchange's
capacity. Samples go into `LatencyHistogram`, one per order, and percentiles are
bucket midpoints within about 3%. The CSV, `BENCH_BUILD/wire_results.csv` by
default, adds `P95_ns`, `P99.99_ns` and `Min_ns`; `Throughput_ops_per_sec` is
the pass rate.

`--core=N` tries to pin the client, not the exchange, to core N (Linux only),
and the table's first line says whether it did.

The run fails on a rejected order, a missing, duplicate or out-of-order reply,
a wrong token or reference, a partial fill, a feed gap or a foreign or malformed
feed packet, 10 s without progress, or an exchange that does not exit 0 on
SIGTERM. SIGHUP, SIGINT, SIGPIPE or SIGTERM to the client kills the exchange and
ends the client by that signal. The CSV rows and the table are written only
after the exchange has exited cleanly.

Known limits:

- Loopback only, one client thread and one session. With a window, latency
  includes queueing in both processes.
- One run at a time per build folder: runs share `wire_latency.conf`.
- The config pre-faults a session store of about 1,280 bytes per order, 64 MB
  at the default `--orders`.
- Unusual launchers are not handled. stdout and stderr must be open at start,
  and a stderr that no one reads fails the run after 10 s. A signal the
  launcher blocks stays blocked, and one it ignores, as `nohup` ignores SIGHUP,
  stays ignored until just before the exchange starts. A `--csv` that names
  `wire_latency.conf` fails the run, and `--csv=/dev/stdout` puts the CSV rows
  before the table.

## Replay and profiling

```sh
make -j4 itch-replay
./build/perf/itch_replay --synth 100000 --validate
```

Replay is a separate tool that drives the same engine. It is not part of this suite.
See [profiling](profiling/README.md) and [current results](../results/README.md).
