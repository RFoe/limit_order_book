#!/usr/bin/env bash
# Index-only replay: v4's flat_map operation sequence for a no-trade workload,
# and synthetic traces with N live keys, to split the index cost into
# instruction work vs memory. Not part of the CMake build.
#   tools/index_trace/build.sh data/itch12302019_AAPL.ops syn:64 syn:27000 ...
# With LOB_PERF_EVENTS="ev1,ev2" each trace also runs under perf stat for those
# events (replay loop only).
source "$(dirname "${BASH_SOURCE[0]}")/../../scripts/common.sh"
here="$LOB_ROOT/tools/index_trace"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
lob_build release >/dev/null
# LOB_IX_STATS=1: build with boost probe statistics (structure only, not timing)
clang++ -std=c++26 -stdlib=libc++ -O3 -march=native -g -DNDEBUG ${LOB_IX_STATS:+-DLOB_IX_STATS} -I "$LOB_ROOT/include" \
    -isystem "$VCPKG_ROOT/installed/x64-linux-clang-libcxx/include" \
    "$here/driver.cxx" "$LOB_ROOT/build/release/liblob_core.a" -lc++abi -o "$out/driver"
mkfifo "$out/ctl" "$out/ack"
exec {ctl_fd}<>"$out/ctl" {ack_fd}<>"$out/ack"
for w in "$@"; do
    taskset -c "$LOB_CPU" "$out/driver" "$w" "${LOB_ROUNDS:-7}"
    if [[ -n "${LOB_PERF_RECORD:-}" ]]; then # e.g. LOB_PERF_RECORD=branch-misses:u, top source lines
        LOB_PERF_CTL_FD=$ctl_fd LOB_PERF_ACK_FD=$ack_fd "$PERF" record -D -1 --control "fd:$ctl_fd,$ack_fd" \
            -e "$LOB_PERF_RECORD" -c "${LOB_PERF_PERIOD:-1000}" -o "$out/perf.data" -- \
            taskset -c "$LOB_CPU" "$out/driver" "$w" 1 >/dev/null 2>&1
        "$PERF" report -i "$out/perf.data" --stdio --no-children -g none --sort srcline --percent-limit 2 2>/dev/null |
            grep -E '^ +[0-9.]+%' | sed "s#^#    $w #"
    fi
    if [[ -n "${LOB_PERF_EVENTS:-}" ]]; then
        LOB_PERF_CTL_FD=$ctl_fd LOB_PERF_ACK_FD=$ack_fd "$PERF" stat -D -1 --control "fd:$ctl_fd,$ack_fd" -x, \
            -e "$LOB_PERF_EVENTS" -- taskset -c "$LOB_CPU" "$out/driver" "$w" 1 2>&1 >/dev/null |
            awk -F, -v w="$w" '{printf "    %-34s %-40s %s\n", w, $3, $1}'
    fi
done
