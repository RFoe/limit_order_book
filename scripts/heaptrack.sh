#!/usr/bin/env bash
# Heap allocation profile of `book replay` (allocation count, peak, hot call sites).
#   scripts/heaptrack.sh [workload.ops] [version]
# Note: the workload buffer itself (24 B/op) is one allocation and shows in the peak.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

workload="${1:-$LOB_DEFAULT_WORKLOAD}"
version="${2:-v0}"
command -v heaptrack >/dev/null || { lob_log "heaptrack missing: sudo apt install heaptrack"; exit 1; }

lob_build release >/dev/null
mkdir -p "$RESULTS_DIR"
stem="$(basename "$workload" .ops)"
base="$RESULTS_DIR/heaptrack_${LOB_PREFIX}_${stem}_${version}"

heaptrack -o "$base" "$(lob_bin release book)" replay --workload "$workload" --version "$version" >/dev/null
heaptrack_print "$base.zst" >"$base.txt"
grep -E '^(total runtime|calls to allocation functions|temporary memory allocations|peak heap memory consumption|peak RSS|total memory leaked)' "$base.txt"
lob_log "wrote $base.txt (raw $base.zst is not committed)"
