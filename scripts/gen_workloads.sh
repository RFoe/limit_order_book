#!/usr/bin/env bash
# Generate the standard synthetic workload set into data/ and record sha256 sums
# (results/workloads_sha256.txt) so other machines can verify they replay
# byte-identical workloads.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

lob_build release >/dev/null
book="$(lob_bin release book)"
mkdir -p "$DATA_DIR" "$RESULTS_DIR"

gen() {
    local name="$1"
    shift
    "$book" gen --out "$DATA_DIR/$name.ops" "$@"
}

# default: ~1.8k live orders, ~44% of ops are cancels, ~5% of pushes cross
gen syn_default_s42 --seed 42
# aggressive: many crossing orders sweeping several levels (matching path)
gen syn_aggressive_s42 --seed 42 --p-marketable 0.25 --cross-ticks 8
# deep: long lifetimes -> ~10x more resting orders (bigger working set)
gen syn_deep_s42 --seed 42 --lifetime 40000

(cd "$DATA_DIR" && sha256sum syn_*.ops) | tee "$RESULTS_DIR/workloads_sha256.txt"
