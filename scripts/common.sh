#!/usr/bin/env bash
# Shared helpers for all lab scripts. Source it; never hard-code machine paths.
set -euo pipefail

_lob_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOB_ROOT="$(git -C "$_lob_script_dir" rev-parse --show-toplevel 2>/dev/null || (cd "$_lob_script_dir/.." && pwd))"
RESULTS_DIR="${LOB_RESULTS_DIR:-$LOB_ROOT/results}"
DATA_DIR="${LOB_DATA_DIR:-$LOB_ROOT/data}"
PERF="${PERF:-perf}"
# Benchmarks are pinned to one CPU (default: the last one).
LOB_CPU="${LOB_CPU:-$(($(nproc) - 1))}"

# vm | metal (override with LOB_ENV_TAG)
lob_env_tag() {
    if [[ -n "${LOB_ENV_TAG:-}" ]]; then
        echo "$LOB_ENV_TAG"
        return
    fi
    local virt=""
    if command -v systemd-detect-virt >/dev/null; then
        virt="$(systemd-detect-virt 2>/dev/null || true)"
    elif grep -qw hypervisor /proc/cpuinfo; then
        virt="unknown-hypervisor"
    fi
    if [[ -z "$virt" || "$virt" == none ]]; then echo metal; else echo vm; fi
}

# short hash, suffixed with -dirty when tracked sources differ from HEAD
# (results/ and docs/ are excluded: writing results must not dirty the tag)
lob_git_hash() {
    local h
    h="$(git -C "$LOB_ROOT" rev-parse --short HEAD 2>/dev/null)" || {
        echo nogit
        return
    }
    if ! git -C "$LOB_ROOT" diff --quiet HEAD -- . ':!results' ':!docs' 2>/dev/null; then
        h="$h-dirty"
    fi
    echo "$h"
}

# <YYYYmmdd-HHMM>_<hash>_<vm|metal>; computed once per script run
LOB_PREFIX="${LOB_PREFIX:-$(date +%Y%m%d-%H%M)_$(lob_git_hash)_$(lob_env_tag)}"

lob_build() { # lob_build <preset>
    local preset="$1"
    (cd "$LOB_ROOT" && cmake --preset "$preset" >/dev/null && cmake --build --preset "$preset")
}

lob_bin() { # lob_bin <preset> <target>
    echo "$LOB_ROOT/build/$1/$2"
}

lob_log() { printf '[%s] %s\n' "$(basename "$0")" "$*" >&2; }

# the default synthetic workload produced by gen_workloads.sh
LOB_DEFAULT_WORKLOAD="${LOB_WORKLOAD:-$DATA_DIR/syn_default_s42.ops}"
