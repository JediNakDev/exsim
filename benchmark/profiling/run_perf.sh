#!/usr/bin/env bash
# Linux profile of one benchmark: perf counters, a call-graph report and a flame graph.
#
# Usage: benchmark/profiling/run_perf.sh [output_dir]
# Build first: make -j4 bench-build
#
# PERF             perf binary (default: perf)
# PERF_FILTER      Google Benchmark regex (default: MixedWorkload)
# PERF_CORE        CPU to pin to (default: 0)
# PERF_EVENT       sampling event (default: cpu-clock:u)
# FLAMEGRAPH_DIR   checkout of github.com/brendangregg/FlameGraph; without it no flame graph is drawn
#
# The samples include the untimed setup between iterations (engine construction,
# prefill, command generation). Read the stacks under MatchingEngine::handle.
set -euo pipefail

BENCH=${BENCH:-build/bench/exsim_benchmark}
PERF=${PERF:-perf}
OUT=${1:-benchmark/profiling/perf_$(date +%Y%m%d_%H%M%S)}
CORE=${PERF_CORE:-0}
FILTER=${PERF_FILTER:-MixedWorkload}
EVENT=${PERF_EVENT:-cpu-clock:u}
RUN=(taskset -c "$CORE" "$BENCH" --core="$CORE" --csv=/dev/null
     --benchmark_filter="$FILTER" --benchmark_repetitions=200)

if [[ ! -x "$BENCH" ]]; then
    echo "error: $BENCH not found; run 'make -j4 bench-build' first" >&2
    exit 1
fi
mkdir -p "$OUT"

# Virtual machines often have no PMU; the counters then print <not supported>.
"$PERF" stat -e cycles,instructions,cache-misses,branch-misses -o "$OUT/perf_stat.txt" -- "${RUN[@]}" > /dev/null
# The bench build keeps frame pointers, so -g unwinds without DWARF. User-only stacks avoid
# the kernel profiling restriction of perf_event_paranoid=2.
"$PERF" record -e "$EVENT" -F 999 -g --user-callchains -o "$OUT/perf.data" -- "${RUN[@]}" > /dev/null
"$PERF" evlist -i "$OUT/perf.data" > "$OUT/events.txt"
"$PERF" report -i "$OUT/perf.data" --stdio --no-children -g none -n --percent-limit=1 > "$OUT/perf_report.txt"

if [[ -n "${FLAMEGRAPH_DIR:-}" ]]; then
    "$PERF" script -i "$OUT/perf.data" | "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" > "$OUT/stacks.folded"
    "$FLAMEGRAPH_DIR/flamegraph.pl" --title "exsim $FILTER" "$OUT/stacks.folded" > "$OUT/flame.svg"
    # Only the stacks under the engine, rooted at handle: no benchmark setup or loop.
    grep 'MatchingEngine::handle(' "$OUT/stacks.folded" | sed 's/^.*;\(exsim::MatchingEngine::handle(\)/\1/' \
        | "$FLAMEGRAPH_DIR/flamegraph.pl" --title "exsim $FILTER, under MatchingEngine::handle" > "$OUT/flame_engine.svg"
fi
echo "wrote $OUT"
