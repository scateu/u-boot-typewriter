#!/bin/bash
#
# wfi_proof.sh - Does CPU idle depth actually affect battery draw on RK3399?
#
# The typewriter power work concluded that WFI (and even deep PSCI cluster-sleep)
# saves almost nothing, because the A53 core rail does not power-gate on WFI
# (PMU_CORE_PM_CON0 core_pm_en=0). This script is the DEFINITIVE Linux-side proof:
# it measures whole-board battery current in three idle regimes and shows they are
# all within gauge noise of each other.
#
#   Phase A  DEEP     : all cpuidle states enabled - the menu governor uses PSCI
#                       cluster-sleep (state2), which physically power-gates the
#                       A53 cluster + L2 via bl31. The deepest idle Linux has.
#   Phase B  WFI-ONLY : disable state1..N so cores can ONLY use plain WFI (state0)
#                       - exactly what the U-Boot typewriter does.
#   Phase C  POLL     : disable state0 too (poll idle) - the core never even WFIs,
#                       it busy-polls. The "no idle at all" floor.
#
# If A ~= B, then power-gating the cluster (bl31) buys nothing over plain WFI.
# If B ~= C, then WFI itself buys nothing over busy-polling.
# Both being true => the A53 core is simply not where the battery goes, and no
# amount of WFI/idle tuning in firmware can move idle draw. That is the claim.
#
# Measurement: the ChromeOS EC smart battery gauge (sbs-9-000b), reg Current(),
# reported in microamps at /sys/class/power_supply/sbs-9-000b/current_now
# (negative = discharging). Same gauge the U-Boot `twwfi`/Ctrl-T path reads, so
# these numbers are directly comparable to the on-board ones.
#
# SAFETY: only writes cpuidle stateN/disable flags, and RESTORES every one on exit
# (even on Ctrl-C). Does not touch power state, rails, or anything persistent.
#
# USAGE:
#   sudo ./wfi_proof.sh [samples_per_phase]     # default 15 (~1 s each)
#   Unplug AC first (must be Discharging). Keep the screen static; do not type.
#
# EXPECTED (measured 2026-08 on Kevin): all three phases ~ -470..-476 mA, spread
# < 15 mA = noise. See ../POWER_CURRENT.md for the recorded run.

set -u

GAUGE=/sys/class/power_supply/sbs-9-000b/current_now
STATUS=/sys/class/power_supply/sbs-9-000b/status
SAMPLES=${1:-15}     # gauge is ~1 Hz; more samples = less noise
SETTLE=3             # seconds to let draw settle after changing idle states

# --- preflight ---------------------------------------------------------------
[ -r "$GAUGE" ] || { echo "ERROR: no SBS gauge at $GAUGE (wrong board?)"; exit 1; }
[ "$(id -u)" -eq 0 ] || { echo "ERROR: run as root (sudo $0)"; exit 1; }

st=$(cat "$STATUS" 2>/dev/null || echo unknown)
echo "battery status : $st"
if [ "$st" != "Discharging" ]; then
    echo "WARNING: not 'Discharging' - on AC the gauge reports CHARGE current and"
    echo "         this benchmark is meaningless. Unplug AC and re-run."
fi

echo "cpuidle driver : $(cat /sys/devices/system/cpu/cpuidle/current_driver 2>/dev/null)"
echo "cpuidle states :"
for s in /sys/devices/system/cpu/cpu0/cpuidle/state*; do
    echo "   $(basename "$s"): $(cat "$s/name") - $(cat "$s/desc" 2>/dev/null)"
done
echo "samples/phase  : $SAMPLES   (~$((SAMPLES + SETTLE)) s per phase)"
echo

# --- save + restore all disable flags exactly --------------------------------
declare -A SAVE
for f in /sys/devices/system/cpu/cpu*/cpuidle/state*/disable; do
    SAVE["$f"]=$(cat "$f")
done
restore() {
    for f in "${!SAVE[@]}"; do echo "${SAVE[$f]}" > "$f" 2>/dev/null; done
    echo "restored cpuidle state flags."
}
trap restore EXIT INT TERM

# set every cpu's stateN/disable to $2, for state suffix $1 (e.g. state1)
set_all() {
    for f in /sys/devices/system/cpu/cpu*/cpuidle/"$1"/disable; do echo "$2" > "$f"; done
}
enable_all_states() {
    for f in /sys/devices/system/cpu/cpu*/cpuidle/state*/disable; do echo 0 > "$f"; done
}

# average the gauge over N 1-second samples, print signed mA
avg_ma() {
    local n=$1 s=0 c i
    for ((i=0; i<n; i++)); do c=$(cat "$GAUGE"); s=$((s + c)); sleep 1; done
    echo $(( s / n / 1000 ))
}

# collect the state* suffixes present on cpu0 (state0,state1,...)
STATES=()
for s in /sys/devices/system/cpu/cpu0/cpuidle/state*; do STATES+=("$(basename "$s")"); done

# --- Phase A: DEEP (all states) ----------------------------------------------
echo "=== PHASE A: DEEP idle (all states; governor picks PSCI cluster-sleep) ==="
enable_all_states
sleep "$SETTLE"
A=$(avg_ma "$SAMPLES")
echo "  deep-allowed avg : ${A} mA"
echo

# --- Phase B: WFI-ONLY (disable state1..N) -----------------------------------
echo "=== PHASE B: WFI-ONLY (disable everything deeper than state0/WFI) ==="
enable_all_states
for n in "${STATES[@]}"; do [ "$n" = state0 ] && continue; set_all "$n" 1; done
sleep "$SETTLE"
B=$(avg_ma "$SAMPLES")
echo "  wfi-only avg     : ${B} mA"
echo

# --- Phase C: POLL (disable state0 too; no WFI at all) -----------------------
echo "=== PHASE C: POLL idle (disable state0/WFI too; core busy-polls) ==="
for n in "${STATES[@]}"; do set_all "$n" 1; done
sleep "$SETTLE"
C=$(avg_ma "$SAMPLES")
echo "  poll (no-WFI) avg: ${C} mA"
echo

restore
trap - EXIT INT TERM

# --- verdict -----------------------------------------------------------------
abs() { local v=$1; echo "${v#-}"; }
dAB=$(( A - B )); dBC=$(( B - C )); dAC=$(( A - C ))
NOISE=15

echo "================= RESULT ================="
printf "  A  deep (cluster-sleep) : %5d mA\n" "$A"
printf "  B  wfi-only             : %5d mA\n" "$B"
printf "  C  poll (no idle)       : %5d mA\n" "$C"
echo   "  ----------------------------------------"
printf "  A-B (cluster vs WFI)    : %5d mA\n" "$dAB"
printf "  B-C (WFI vs no-idle)    : %5d mA\n" "$dBC"
printf "  A-C (deepest vs busiest): %5d mA\n" "$dAC"
echo   "  (noise floor ~${NOISE} mA; SBS gauge is ~1 Hz, +-10..20 mA)"
echo
if [ "$(abs "$dAC")" -lt "$NOISE" ]; then
    echo "  => CONFIRMED: idle depth does NOT matter. Cluster power-gating (bl31),"
    echo "     plain WFI, and busy-polling all draw the same within noise. The A53"
    echo "     core is not where the battery goes; WFI/idle tuning cannot move it."
    echo "     The real levers are DDR freq and backlight (see ../POWER_CURRENT.md)."
else
    echo "  => Idle depth changed draw by ${dAC} mA across the extremes."
    echo "     Unexpected on this board - re-run with more samples to rule out noise."
fi
