# Results

One result set, measured on the code in this branch. Rerun after any engine change; old numbers are not kept.

| File | What | How |
| --- | --- | --- |
| `macos_arm64.csv` | Latency and throughput of the six benchmark cases | `make bench ARGS=--csv=benchmark/results/macos_arm64.csv` |
| `linux_arm64_flame.svg` | Flame graph of `MixedWorkload` | `benchmark/profiling/run_perf.sh` |
| `linux_arm64_flame_engine.svg` | The same samples, only the stacks under `MatchingEngine::handle` | same run |
| `linux_arm64_perf_report.txt` | Functions above 1% of samples in the same profile | same run |

The Linux profile was measured 2026-11-18 with fixed `EventBuffer` storage (#49), 10,388 samples. `MatchingEngine::handle` is 73% of them; inside it, `handle` itself is 40%, `match_and_dispose` 22%, `rest` 20%, `remove_resting` 8%, `append_event` 8%.

The macOS CSV was measured 2026-11-17, before #49. The host was too busy on 2026-11-18 to replace it fairly: busy runs were slower than this quiet one even with the faster code. The A/B in #49 has the change at 1% to 16% faster on macOS. Rerun it on an idle machine.

- macOS: Apple M6, macOS 27.0.1, Apple clang 21. The thread could not be pinned; macOS only takes an affinity hint.
- Linux: Ubuntu 24.04 in an OrbStack virtual machine on the same host, g++ 13.3, perf 6.8. The machine has no PMU, so the profile samples `cpu-clock` and there are no cycle or cache-miss counters.

CSV percentiles are percentiles of 32-command batch means, not single-command tail latency. See [the benchmark suite](../README.md) for what is and is not measured.

`linux_arm64_flame.svg` includes the untimed setup between iterations; `linux_arm64_flame_engine.svg` does not.
