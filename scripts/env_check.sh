#!/usr/bin/env bash
# Read-only snapshot of everything that affects measurement quality.
# Writes results/env_<date>_<hash>_<vm|metal>.txt. Never changes the system.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

mkdir -p "$RESULTS_DIR"
out="$RESULTS_DIR/env_${LOB_PREFIX}.txt"

rd() { if [[ -r "$1" ]]; then tr '\n' ' ' <"$1" | sed 's/ *$//'; else echo "n/a"; fi; }
show() { printf '%-26s %s\n' "$1" "$2"; }
section() { printf '\n## %s\n' "$1"; }
has_flag() { grep -m1 '^flags' /proc/cpuinfo | grep -qw "$1" && echo yes || echo NO; }

{
    echo "# environment snapshot $LOB_PREFIX"

    section host
    show env_tag "$(lob_env_tag)"
    show virt "$(systemd-detect-virt 2>/dev/null || echo none)"
    show kernel "$(uname -r)"
    show cpu "$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    show nproc "$(nproc)"
    show bench_cpu "$LOB_CPU (LOB_CPU)"
    show caches "$(lscpu | awk -F: '/^L1d|^L2|^L3/ {gsub(/^ +/, "", $2); printf "%s=%s; ", $1, $2}')"
    show git "$(lob_git_hash)"

    section "frequency scaling"
    show scaling_driver "$(rd /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver)"
    show governor "$(rd /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
    show intel_pstate.no_turbo "$(rd /sys/devices/system/cpu/intel_pstate/no_turbo)"
    show cpufreq.boost "$(rd /sys/devices/system/cpu/cpufreq/boost)"
    show cpu_mhz "$(grep '^cpu MHz' /proc/cpuinfo | awk '{printf "%s ", $4}')"

    section "idle states"
    show cpuidle.driver "$(rd /sys/devices/system/cpu/cpuidle/current_driver)"
    show cpuidle.governor "$(rd /sys/devices/system/cpu/cpuidle/current_governor)"
    for s in /sys/devices/system/cpu/cpu0/cpuidle/state*; do
        [[ -d "$s" ]] && show "  $(basename "$s")" "$(rd "$s/name") disabled=$(rd "$s/disable") latency_us=$(rd "$s/latency")"
    done

    section isolation
    show cmdline "$(rd /proc/cmdline)"
    show isolated "$(rd /sys/devices/system/cpu/isolated)"
    show nohz_full "$(rd /sys/devices/system/cpu/nohz_full)"

    section memory
    show thp.enabled "$(rd /sys/kernel/mm/transparent_hugepage/enabled)"
    show thp.defrag "$(rd /sys/kernel/mm/transparent_hugepage/defrag)"
    show hugepages "$(awk '/HugePages_Total|Hugepagesize/ {printf "%s %s %s; ", $1, $2, $3}' /proc/meminfo)"
    show mem_total "$(awk '/MemTotal/ {print $2, $3}' /proc/meminfo)"

    section "perf / PMU"
    show perf_event_paranoid "$(rd /proc/sys/kernel/perf_event_paranoid)"
    show kptr_restrict "$(rd /proc/sys/kernel/kptr_restrict)"
    show nmi_watchdog "$(rd /proc/sys/kernel/nmi_watchdog)"
    if command -v "$PERF" >/dev/null; then
        show perf "$("$PERF" --version 2>&1)"
        pmu="$("$PERF" stat -x, -e cycles,instructions true 2>&1 | grep -E 'cycles|instructions' | cut -d, -f1,3 | tr '\n' ' ')"
        show "pmu (cycles,instructions)" "$pmu"
        if grep -q 'not supported' <<<"$pmu"; then
            show pmu_verdict "UNAVAILABLE -> use cachegrind Ir/D1/LL as primary metric"
        elif grep -qE '^[0-9]' <<<"$pmu"; then
            show pmu_verdict "available"
        else
            show pmu_verdict "unknown (permission? see perf_event_paranoid)"
        fi
    else
        show perf "not installed"
    fi

    section TSC
    for f in constant_tsc nonstop_tsc rdtscp tsc_known_freq; do show "$f" "$(has_flag $f)"; done
    show clocksource "$(rd /sys/devices/system/clocksource/clocksource0/current_clocksource)"

    section tools
    show clang++ "$(clang++ --version 2>/dev/null | head -1)"
    show cmake "$(cmake --version 2>/dev/null | head -1)"
    show valgrind "$(valgrind --version 2>/dev/null || echo missing)"
    show heaptrack "$(heaptrack --version 2>/dev/null | head -1 || echo missing)"
    show VCPKG_ROOT "${VCPKG_ROOT:-unset}"
} | tee "$out"

lob_log "wrote $out"
