#!/usr/bin/env bash
# Google Benchmark over every registered version, pinned to one CPU.
#   scripts/bench.sh [extra google-benchmark flags]
#   LOB_WORKLOAD=data/a.ops,data/b.ops scripts/bench.sh
# Repetitions of different versions are randomly interleaved so they share the
# same noise. Read the median and the CV, not the mean.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

lob_build release >/dev/null
workloads="${LOB_WORKLOAD:-$LOB_DEFAULT_WORKLOAD}"
mkdir -p "$RESULTS_DIR"
out="$RESULTS_DIR/bench_${LOB_PREFIX}.json"

lob_log "cpu=$LOB_CPU workloads=$workloads"
taskset -c "$LOB_CPU" "$(lob_bin release benchmark_book)" \
    --workload="$workloads" \
    --benchmark_repetitions="${LOB_REPS:-20}" \
    --benchmark_report_aggregates_only=true \
    --benchmark_enable_random_interleaving=true \
    --benchmark_out="$out" --benchmark_out_format=json \
    "$@" | tee "${out%.json}.txt"
lob_log "wrote $out"
