#!/usr/bin/env bash
# Per-op latency distribution (rdtscp), versions interleaved round by round.
#   scripts/latency.sh [workload.ops] [versions]      (default: syn_default, all)
# In a VM only relative comparisons are meaningful.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

workload="${1:-$LOB_DEFAULT_WORKLOAD}"
versions="${2:-all}"
lob_build release >/dev/null
mkdir -p "$RESULTS_DIR"
stem="$(basename "$workload" .ops)"
out="$RESULTS_DIR/latency_${LOB_PREFIX}_${stem}"

taskset -c "$LOB_CPU" "$(lob_bin release book)" latency --workload "$workload" --versions "$versions" \
    --rounds "${LOB_ROUNDS:-5}" --csv "$out.csv" | tee "$out.txt"
