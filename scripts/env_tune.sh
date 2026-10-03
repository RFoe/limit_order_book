#!/usr/bin/env bash
# Put the machine into a quieter benchmarking state, and undo it.
#   scripts/env_tune.sh apply     (saves the previous values first)
#   scripts/env_tune.sh restore
# Uses sudo (you will be prompted). Knobs that do not exist on this machine
# (e.g. cpufreq inside a VM) are skipped. Boot-time settings are only printed.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

state_dir="${XDG_STATE_HOME:-$HOME/.local/state}/lob"
state="$state_dir/env_tune.saved"

knobs() { # path <TAB> value
    printf '%s\t%s\n' /proc/sys/kernel/perf_event_paranoid -1
    printf '%s\t%s\n' /proc/sys/kernel/kptr_restrict 0
    printf '%s\t%s\n' /proc/sys/kernel/nmi_watchdog 0
    printf '%s\t%s\n' /sys/devices/system/cpu/intel_pstate/no_turbo 1
    printf '%s\t%s\n' /sys/devices/system/cpu/cpufreq/boost 0
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        printf '%s\t%s\n' "$g" performance
    done
}

write() { echo "$2" | sudo tee "$1" >/dev/null; }

case "${1:-}" in
apply)
    mkdir -p "$state_dir"
    : >"$state"
    while IFS=$'\t' read -r path value; do
        if [[ ! -e "$path" ]]; then
            lob_log "skip (not present): $path"
            continue
        fi
        printf '%s\t%s\n' "$path" "$(cat "$path")" >>"$state"
        write "$path" "$value" && lob_log "set $path = $value"
    done < <(knobs)
    lob_log "previous values saved to $state"
    cat <<EOF

Boot-time knobs (not changed by this script; edit GRUB_CMDLINE_LINUX in
/etc/default/grub, run 'sudo update-grub' and reboot):
  isolcpus=$LOB_CPU nohz_full=$LOB_CPU rcu_nocbs=$LOB_CPU   # keep the scheduler and ticks off the bench CPU
  intel_idle.max_cstate=0 processor.max_cstate=1           # shallow C-states (bare metal only)
  default_hugepagesz=2M hugepages=512                      # reserve huge pages
EOF
    ;;
restore)
    [[ -f "$state" ]] || { lob_log "nothing saved at $state"; exit 1; }
    while IFS=$'\t' read -r path value; do
        write "$path" "$value" && lob_log "restored $path = $value"
    done <"$state"
    rm -f "$state"
    ;;
*)
    echo "usage: $0 apply|restore" >&2
    exit 2
    ;;
esac
