#!/usr/bin/env bash
# perf wrappers around `book replay`.
#   scripts/perf.sh record [workload.ops] [version]   sampling profile + report/annotate text
#   scripts/perf.sh stat   [workload.ops] [version]   software counters (5 runs)
#   scripts/perf.sh trace  [workload.ops] [version]   syscall summary (needs sudo: tracefs)
# Hardware events are probed once; if the PMU is unavailable (typical in a VM)
# sampling falls back to the cpu-clock software event.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

mode="${1:-record}"
workload="${2:-$LOB_DEFAULT_WORKLOAD}"
version="${3:-v0}"
repeat="${LOB_REPEAT:-10}" # amortise workload loading

lob_build release >/dev/null
book="$(lob_bin release book)"
mkdir -p "$RESULTS_DIR"
stem="$(basename "$workload" .ops)"
base="$RESULTS_DIR/perf_${mode}_${LOB_PREFIX}_${stem}_${version}"
cmd=(taskset -c "$LOB_CPU" "$book" replay --workload "$workload" --version "$version" --repeat "$repeat")

if "$PERF" stat -x, -e cycles true 2>&1 | grep -q '^[0-9]'; then
    event=cycles
else
    event=cpu-clock
fi
# user space only by default: in a VM, cpu-clock samples pile up on kernel
# irq-restore points and drown the profile (override with LOB_PERF_EVENT)
event="${LOB_PERF_EVENT:-$event:u}"

case "$mode" in
record)
    lob_log "sampling event: $event"
    "$PERF" record -e "$event" -F "${LOB_PERF_FREQ:-4999}" --call-graph dwarf -o "$base.perf.data" -- "${cmd[@]}"
    {
        echo "# event=$event workload=$stem version=$version repeat=$repeat"
        "$PERF" report -i "$base.perf.data" --stdio --no-children --percent-limit 0.5 2>/dev/null
    } >"$base.report.txt"
    "$PERF" annotate -i "$base.perf.data" --stdio 2>/dev/null | head -n 2000 >"$base.annotate.txt"
    lob_log "wrote $base.report.txt / .annotate.txt (raw: $base.perf.data, not committed)"
    ;;
stat)
    "$PERF" stat -r 5 -e task-clock,page-faults,minor-faults,major-faults,context-switches,cpu-migrations \
        -- "${cmd[@]}" 2>&1 >/dev/null | tee "$base.txt"
    ;;
trace)
    sudo "$PERF" trace -s -- "${cmd[@]}" 2>&1 >/dev/null | tee "$base.txt"
    ;;
*)
    echo "usage: $0 record|stat|trace [workload] [version]" >&2
    exit 2
    ;;
esac
