#!/usr/bin/env bash
# Deterministic instruction / cache-miss counts: the primary metric in a VM.
#   scripts/cachegrind.sh [workload.ops] [version ...]     (default: syn_default, all versions)
# Only the replay loop is counted (CACHEGRIND_START/STOP_INSTRUMENTATION in
# `book replay`). The cache geometry is pinned (LOB_CG_CACHE) so numbers do not
# depend on what CPUID the VM / host reports.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

workload="${1:-$LOB_DEFAULT_WORKLOAD}"
shift || true

lob_build profile >/dev/null
# valgrind 3.22 dies with SIGILL on AVX-512 (EVEX) code: refuse native builds
if grep -q '^LOB_MARCH:STRING=native' "$LOB_ROOT/build/profile/CMakeCache.txt"; then
    lob_log "build/profile was configured with -march=native; valgrind cannot run it"
    exit 1
fi
book="$(lob_bin profile book)"
versions=("$@")
((${#versions[@]})) || mapfile -t versions < <("$book" versions)

# i5-1035G1 (Ice Lake client) geometry: 32K/8w I1, 48K/12w D1, 6M/12w LL
cache="${LOB_CG_CACHE:---I1=32768,8,64 --D1=49152,12,64 --LL=6291456,12,64}"
mkdir -p "$RESULTS_DIR"
stem="$(basename "$workload" .ops)"
summary="$RESULTS_DIR/cachegrind_${LOB_PREFIX}_${stem}.txt"
echo "# cachegrind $LOB_PREFIX workload=$stem cache: $cache" >"$summary"

for v in "${versions[@]}"; do
    base="$RESULTS_DIR/cachegrind_${LOB_PREFIX}_${stem}_${v}"
    lob_log "$v on $stem"
    # shellcheck disable=SC2086
    valgrind --tool=cachegrind --cache-sim=yes --instr-at-start=no $cache \
        --cachegrind-out-file="$base.out" \
        "$book" replay --workload "$workload" --version "$v" 2>"$base.log" | tee -a "$summary"
    {
        echo "## $v"
        grep -E '(I|D|LL) +(refs|misses)|(I1|LLi|D1|LLd|LL) +miss rate|LL refs' "$base.log" | sed 's/^==[0-9]*== //'
    } | tee -a "$summary"
    cg_annotate "$base.out" >"$base.annotate.txt"
done
lob_log "wrote $summary (+ per-version .out/.annotate.txt; compare versions with cg_diff)"
