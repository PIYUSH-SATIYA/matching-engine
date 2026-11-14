#!/usr/bin/env bash
# Unattended, single-command benchmark session.
#
#   ./scripts/run_session.sh
#
# What it does, in order:
#   1. refuses to start unless the git tree is clean (data must be attributable
#      to a commit) and warns if the host is already busy;
#   2. switches the platform power profile to "performance" and records which
#      profile actually took effect;
#   3. moves the previous raw data aside (results/raw-prev-<ts>) so the old
#      session stays available for comparison;
#   4. rebuilds Release;
#   5. runs the whole suite: environment capture, 8-configuration matrix,
#      allocator experiments + SCHED_FIFO probe, saturation sweep, component
#      micro-benchmarks, mechanism probes, then both analysis scripts;
#   6. appends a post-run thermal/frequency snapshot to results/env.txt.
#
# A failing phase does not abort the rest of the session: every phase exit code
# is recorded and printed at the end, so one bad probe cannot cost the whole
# run.  All output is mirrored to results/bench-session-<ts>.log.
#
# Environment overrides: ME_LAT_RUNS (10), ME_THR_RUNS (5), ME_SAT_REPS (3),
# ME_COMPONENT_REPS (9), ME_PROBE_REPS (3), ME_PIN_NET (0), ME_SESSION_LOG.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
RES=${RESULTS:-"$ROOT/results"}

TS=$(date +%Y%m%dT%H%M%S)

# Re-exec once under tee so the transcript is saved without losing buffered
# output at exit (a bare `exec > >(tee ...)` can truncate the tail).
if [ -z "${ME_SESSION_LOGGED:-}" ]; then
  export ME_SESSION_LOGGED=1
  export ME_SESSION_LOG="${ME_SESSION_LOG:-$RES/bench-session-$TS.log}"
  mkdir -p "$RES"
  "$0" "$@" 2>&1 | tee "$ME_SESSION_LOG"
  exit "${PIPESTATUS[0]}"
fi
LOG=${ME_SESSION_LOG:-}

mkdir -p "$RES"
echo "=== benchmark session $TS ==="
echo "repo=$ROOT"
echo "log=$LOG"

# ---------------------------------------------------------------- preconditions
die() { echo "ABORT: $*"; exit 1; }

branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null) || die "not a git checkout"
commit=$(git rev-parse --short HEAD)
dirty=$(git status --porcelain -- . ':!results' ':!build*')
if [ -n "$dirty" ]; then
  echo "$dirty"
  die "working tree has uncommitted changes; commit them first"
fi
echo "branch=$branch commit=$commit"

load1=$(cut -d' ' -f1 /proc/loadavg)
echo "loadavg=$(cut -d' ' -f1-3 /proc/loadavg)"
if awk -v l="$load1" 'BEGIN{exit !(l > 1.0)}'; then
  echo "WARNING: load1=$load1 > 1.0 - something else is running."
  echo "         Browsers, video and builds must be closed for a usable run."
fi
echo "-- top CPU consumers right now --"
ps -eo pcpu,comm --sort=-pcpu | head -6

# ------------------------------------------------------------- power profile
if command -v powerprofilesctl >/dev/null 2>&1; then
  powerprofilesctl set performance 2>/dev/null ||
    echo "WARNING: could not set the performance profile (needs a local session)"
fi
profile=$(cat /sys/firmware/acpi/platform_profile 2>/dev/null || echo n/a)
echo "platform_profile=$profile"
if [ "$profile" != "performance" ]; then
  echo "WARNING: profile is '$profile', not 'performance'."
  echo "         Absolute latencies will not be comparable with a performance run."
fi

echo
echo "Starting in 5s. Press Ctrl-C now to abort."
for i in 5 4 3 2 1; do
  printf '\r  %ss ' "$i"
  sleep 1
done
printf '\r\n'

# ----------------------------------------------------------- preserve old data
if [ -d "$RES/raw" ] && [ -n "$(ls -A "$RES/raw" 2>/dev/null)" ]; then
  mv "$RES/raw" "$RES/raw-prev-$TS"
  echo "previous raw data -> $RES/raw-prev-$TS"
fi
mkdir -p "$RES/raw"
if [ -d "$RES/components" ]; then
  cp -a "$RES/components" "$RES/components-prev-$TS"
  echo "previous component data -> $RES/components-prev-$TS"
fi

# ---------------------------------------------------------------------- build
echo
echo "=== build (Release) ==="
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release >/dev/null ||
  die "cmake configure failed"
cmake --build build-rel -j"$(nproc)" || die "build failed"

# ------------------------------------------------------- background telemetry
# A 5 s sampler answers "did the machine throttle during the session?" without
# perturbing it (one cat per second per file at most).  If the peak package
# temperature approaches Tjmax the late configurations are slower than the
# early ones, which would bias the matrix ordering.
THERMAL="$RES/thermal-$TS.csv"
temperature_sampler() {
  local f pkg_zone="" hi lo m
  for f in /sys/class/thermal/thermal_zone*/type; do
    case "$(cat "$f" 2>/dev/null)" in
      x86_pkg_temp) pkg_zone=$(dirname "$f")/temp; break ;;
    esac
  done
  echo "ts,profile,pkg_temp_c,max_mhz_p,min_mhz_p,load1" >"$THERMAL"
  while :; do
    hi=0
    lo=999999
    for f in /sys/devices/system/cpu/cpu[0-7]/cpufreq/scaling_cur_freq; do
      m=$(cat "$f" 2>/dev/null) || continue
      [ "$m" -gt "$hi" ] && hi=$m
      [ "$m" -lt "$lo" ] && lo=$m
    done
    echo "$(date +%H:%M:%S),$(cat /sys/firmware/acpi/platform_profile 2>/dev/null || echo n/a),$(
      awk -v t="$(cat "$pkg_zone" 2>/dev/null || echo 0)" 'BEGIN{printf "%.1f", t/1000}'
    ),$((hi / 1000)),$((lo / 1000)),$(cut -d' ' -f1 /proc/loadavg)" >>"$THERMAL"
    sleep 5
  done
}
temperature_sampler &
SAMPLER_PID=$!
trap 'kill "$SAMPLER_PID" 2>/dev/null' EXIT

# --------------------------------------------------------------------- phases
declare -A RC
phase() {
  local name=$1
  shift
  echo
  echo "=== phase: $name ==="
  local t0=$SECONDS
  "$@"
  RC[$name]=$?
  echo "=== phase: $name rc=${RC[$name]} elapsed=$((SECONDS - t0))s ==="
}

phase env_record ./scripts/env.sh
phase matrix env ME_LAT_RUNS="${ME_LAT_RUNS:-10}" ME_THR_RUNS="${ME_THR_RUNS:-5}" \
  ./scripts/run_matrix.sh
phase allocator_experiments ./scripts/run_experiments.sh
phase saturation ./scripts/run_saturation.sh
phase components ./scripts/run_components.sh
phase probes ./scripts/run_probe.sh
phase analyze python3 scripts/analyze.py
phase analyze_components python3 scripts/analyze_components.py

# --------------------------------------------------------- post-run telemetry
echo
echo "=== post-run state ==="
{
  echo
  echo "## Post-run state ($TS)"
  date -Is
  echo "platform_profile=$(cat /sys/firmware/acpi/platform_profile 2>/dev/null || echo n/a)"
  echo "loadavg=$(cat /proc/loadavg)"
  for zone in /sys/class/thermal/thermal_zone*; do
    [ -r "$zone/temp" ] || continue
    echo "temp $(cat "$zone/type" 2>/dev/null || basename "$zone") = $(cat "$zone/temp")mC"
  done
  echo "-- scaling_cur_freq (kHz) sampled after the run"
  for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq; do
    [ -r "$f" ] || continue
    echo "$(basename "$(dirname "$(dirname "$f")")") $(cat "$f")"
  done
} | tee -a "$RES/env.txt"

# ---------------------------------------------------------------------- report
echo
echo "=== summary ==="
fail=0
for name in env_record matrix allocator_experiments saturation components \
  probes analyze analyze_components; do
  rc=${RC[$name]:-missing}
  printf '  %-22s rc=%s\n' "$name" "$rc"
  [ "$rc" = "0" ] || fail=1
done
rows=$(awk 'FNR > 1' "$RES"/raw/*/runs.csv 2>/dev/null | wc -l)
echo "  result rows            $rows"
if [ -s "$THERMAL" ]; then
  awk -F, 'NR > 1 && $3 > m {m = $3} END {printf "  peak pkg temp          %.1f C\n", m}' \
    "$THERMAL"
  echo "  telemetry              $THERMAL"
fi
echo
if [ "$RES" = "$ROOT/results" ]; then
  echo "-- tracked files changed under results/ --"
  git status --porcelain -- results | head -20
fi
echo
echo "session log: ${LOG:-none}"
if [ "$fail" -eq 0 ]; then
  echo "ALL PHASES OK"
else
  echo "SOME PHASES FAILED - check the rc values above"
fi
echo "power profile left at 'performance'; use 'powerprofilesctl set balanced' to restore"
exit "$fail"
