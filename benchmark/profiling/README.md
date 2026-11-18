# Profiling

Build with `make -j4 bench-build`, then run:

```sh
benchmark/profiling/run_macos.sh output/macos-profile
FLAMEGRAPH_DIR=/path/to/FlameGraph benchmark/profiling/run_perf.sh output/perf-profile
```

Linux needs `perf`, `taskset` and permission to sample. Hardware counters are
needed for the counter report; software sampling can still work without a PMU.
`FLAMEGRAPH_DIR` points to Brendan Gregg's FlameGraph tools. The script writes
counter output, a sampled call graph, a text report and, when the tools are
provided, `flame.svg` and `flame_engine.svg` (only the stacks under
`MatchingEngine::handle`). A failed counter or sampling command stops the script.
It does not substitute fabricated counters or stacks.

`BENCH` selects the binary, `PERF` selects perf, `PERF_FILTER` selects cases,
`PERF_CORE` selects the Linux CPU, and `PERF_EVENT` selects the sampling event
(default `cpu-clock:u`). `events.txt` records the actual sampling event;
perf can substitute events on unsupported hardware. Scripts repeat the fixed-iteration cases to collect more
samples. Whole-process stacks include setup; inspect stacks under
`MatchingEngine::handle` to distinguish matching from allocation and generation.

macOS uses `xcrun xctrace` and `/usr/bin/sample`; CPU counters need Apple
developer tools and permissions. Pinning on macOS is only a hint.

Keep the revision, flags, workload, hardware and profiler settings with captures.
No old captures are retained as evidence of current performance.
