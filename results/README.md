# Results

One result set, measured on the code in this branch. Rerun after any engine change; old numbers are not kept.

| File | What | How |
| --- | --- | --- |
| `macos_arm64.csv` | Latency and throughput of the six benchmark cases | `make bench ARGS=--csv=results/macos_arm64.csv` |
| `linux_arm64_flame.svg` | Flame graph of `MixedWorkload` | `benchmark/profiling/run_perf.sh` |
| `linux_arm64_perf_report.txt` | Functions above 1% of samples in the same profile | same run |

Measured 2026-11-17, after the review fixes (#11 to #19) and the CI fixes.

- macOS: Apple M6, macOS 27.0.1, Apple clang 21. The thread could not be pinned; macOS only takes an affinity hint.
- Linux: Ubuntu 24.04 in an OrbStack virtual machine on the same host, g++ 13.3, perf 6.8. The machine has no PMU, so the profile samples `cpu-clock` and there are no cycle or cache-miss counters.

CSV percentiles are percentiles of 32-command batch means, not single-command tail latency. See [the benchmark suite](../benchmark/README.md) for what is and is not measured.

The profile includes the untimed setup between iterations. Read the stacks under `MatchingEngine::handle`.
