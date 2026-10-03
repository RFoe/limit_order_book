#!/usr/bin/env bash
# perf wrappers around `book replay`, counting ONLY the replay loop: perf starts
# disabled (-D -1) and `book replay` toggles it through --control fd:CTL,ACK
# (env LOB_PERF_CTL_FD/LOB_PERF_ACK_FD), so workload loading is excluded.
#
#   scripts/perf.sh record  [workload.ops] [version]   sampling profile -> report/annotate text
#   scripts/perf.sh stat    [workload.ops] [version]   hardware counters in passes of <=4 events
#   scripts/perf.sh topdown [workload.ops] [version]   approximate TopDown level 1 from raw events
#   scripts/perf.sh trace   [workload.ops] [version]   syscall summary (sudo: tracefs; whole process)
#
# The PMU is probed once. Without it, record falls back to cpu-clock and
# stat/topdown are unavailable. Event names in stat/topdown are Ice Lake ones;
# on another micro-architecture check `perf list` and override with
# LOB_PERF_PASSES="ev1,ev2;ev3,ev4" (one pass per ';').
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

mode="${1:-record}"
workload="${2:-$LOB_DEFAULT_WORKLOAD}"
version="${3:-v0}"
repeat="${LOB_REPEAT:-5}"

lob_build release >/dev/null
book="$(lob_bin release book)"
mkdir -p "$RESULTS_DIR"
stem="$(basename "$workload" .ops)"
base="$RESULTS_DIR/perf_${mode}_${LOB_PREFIX}_${stem}_${version}"
cmd=(taskset -c "$LOB_CPU" "$book" replay --workload "$workload" --version "$version")

if "$PERF" stat -x, -e cycles true 2>&1 | grep -q '^[0-9]'; then pmu=1; else pmu=0; fi

# ---- region-of-interest control channel -------------------------------------
roi_dir="$(mktemp -d)"
trap 'rm -rf "$roi_dir"' EXIT
mkfifo "$roi_dir/ctl" "$roi_dir/ack"
exec {ctl_fd}<>"$roi_dir/ctl" {ack_fd}<>"$roi_dir/ack"
export LOB_PERF_CTL_FD="$ctl_fd" LOB_PERF_ACK_FD="$ack_fd"
roi=(-D -1 --control "fd:$ctl_fd,$ack_fd")

need_pmu() { ((pmu)) || { lob_log "hardware PMU unavailable (see scripts/env_check.sh)"; exit 1; }; }

case "$mode" in
record)
    # user space only: in a VM without a PMU, cpu-clock samples pile up on kernel
    # irq-restore points. No PEBS in this VM, so cycles samples have skid.
    if ((pmu)); then event=cycles; else event=cpu-clock; fi
    event="${LOB_PERF_EVENT:-$event:u}"
    lob_log "sampling event: $event (repeat=$repeat, ${LOB_PERF_PERIOD:+period=$LOB_PERF_PERIOD}${LOB_PERF_PERIOD:-freq=${LOB_PERF_FREQ:-4999}})"
    # rare events (e.g. mem_load_retired.l3_miss) need a fixed period: LOB_PERF_PERIOD=N -> -c N
    if [[ -n "${LOB_PERF_PERIOD:-}" ]]; then rate=(-c "$LOB_PERF_PERIOD"); else rate=(-F "${LOB_PERF_FREQ:-4999}"); fi
    "$PERF" record "${roi[@]}" -e "$event" "${rate[@]}" --call-graph dwarf \
        -o "$base.perf.data" -- "${cmd[@]}" --repeat "$repeat" >/dev/null
    {
        echo "# event=$event workload=$stem version=$version repeat=$repeat (replay region only)"
        "$PERF" report -i "$base.perf.data" --stdio --no-children --percent-limit 0.5 2>/dev/null
    } >"$base.report.txt"
    "$PERF" annotate -i "$base.perf.data" --stdio 2>/dev/null | head -n 2000 >"$base.annotate.txt"
    lob_log "wrote $base.report.txt / .annotate.txt (raw: $base.perf.data, not committed)"
    ;;
stat)
    need_pmu
    # <=4 programmable events per pass (some, e.g. cycle_activity.*, are
    # restricted to a subset of counters) so nothing is multiplexed
    passes="${LOB_PERF_PASSES:-cycles,instructions,branches,branch-misses;L1-dcache-loads,mem_load_retired.l1_miss,mem_load_retired.l2_miss,mem_load_retired.l3_miss;cycles,cycle_activity.stalls_mem_any,cycle_activity.stalls_l3_miss,dtlb_load_misses.walk_completed}"
    : >"$base.txt"
    IFS=';' read -ra pass_list <<<"$passes"
    for p in "${pass_list[@]}"; do
        evs="$(sed -E 's/([^,]+)/\1:u/g' <<<"$p")"
        "$PERF" stat "${roi[@]}" -r "$repeat" -e "$evs" -- "${cmd[@]}" 2>&1 >/dev/null |
            grep -E '^ +[0-9<]' | tee -a "$base.txt"
    done
    echo "# replay region only; user space; mean of $repeat runs; ops=$("$book" info "$workload" | awk -F'n_ops=' 'NR==1{split($2,a," "); print a[1]}')" | tee -a "$base.txt"
    ;;
topdown)
    need_pmu
    # TMA level 1 without perf-metrics (not virtualised here), from raw events:
    #   FE  = IDQ_UOPS_NOT_DELIVERED.CORE / SLOTS
    #   BS  = (UOPS_ISSUED.ANY - UOPS_RETIRED.SLOTS + W * INT_MISC.RECOVERY_CYCLES) / SLOTS
    #   RET = UOPS_RETIRED.SLOTS / SLOTS ;  BE = 1 - FE - BS - RET
    # W (pipeline width) = SLOTS / CYCLES, measured. Approximate: not cross-checked
    # against the hardware perf-metrics.
    evs="cpu_clk_unhalted.thread:u,topdown.slots:u,uops_issued.any:u,uops_retired.slots:u,idq_uops_not_delivered.core:u,int_misc.recovery_cycles:u"
    "$PERF" stat "${roi[@]}" -x, -r "$repeat" -e "$evs" -- "${cmd[@]}" 2>&1 >/dev/null |
        awk -F, '
            $3 ~ /cpu_clk_unhalted.thread/ {cyc=$1} $3 ~ /topdown.slots/ {slots=$1}
            $3 ~ /uops_issued.any/ {iss=$1}        $3 ~ /uops_retired.slots/ {ret=$1}
            $3 ~ /idq_uops_not_delivered/ {fe=$1}  $3 ~ /recovery_cycles/ {rec=$1}
            {print "# " $0}
            END {
                if (slots == 0 || cyc == 0) { print "topdown events not available"; exit 1 }
                w = int(slots / cyc + 0.5)
                FE = fe / slots; BS = (iss - ret + w * rec) / slots; RET = ret / slots; BE = 1 - FE - BS - RET
                printf "pipeline width W = %d (slots/cycles = %.3f)\n", w, slots / cyc
                printf "Frontend_Bound   %5.1f%%\nBad_Speculation  %5.1f%%\nRetiring         %5.1f%%\nBackend_Bound    %5.1f%%\n", 100*FE, 100*BS, 100*RET, 100*BE
            }' | tee "$base.txt"
    ;;
trace)
    sudo "$PERF" trace -s -- "${cmd[@]}" 2>&1 >/dev/null | tee "$base.txt"
    ;;
*)
    echo "usage: $0 record|stat|topdown|trace [workload] [version]" >&2
    exit 2
    ;;
esac
