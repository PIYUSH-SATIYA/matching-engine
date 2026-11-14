#!/usr/bin/env bash
# Capture the environment once per benchmark session.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RESULTS=${RESULTS:-"$ROOT/results"}
mkdir -p "$RESULTS"
OUT="$RESULTS/env.txt"

{
  echo "# Benchmark environment"
  date -Is
  echo "git_branch=$(git -C "$ROOT" branch --show-current)"
  echo "git_commit=$(git -C "$ROOT" rev-parse HEAD)"
  echo "git_dirty=$(git -C "$ROOT" status --porcelain -- . ':!results' ':!build*' | tr '\n' ';')"
  echo "build_type=Release"
  echo
  echo "## Host"
  uname -a
  echo
  echo "## CPU topology (lscpu -e)"
  lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE,MAXMHZ,MINMHZ
  echo
  echo "## Governor / turbo / SMT"
  for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
    [ -r "$cpu/cpufreq/scaling_governor" ] || continue
    echo "$(basename "$cpu") $(cat "$cpu/cpufreq/scaling_governor")"
  done
  if [ -r /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    echo "intel_pstate/no_turbo=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)"
  fi
  [ -r /sys/devices/system/cpu/smt/active ] &&
    echo "smt/active=$(cat /sys/devices/system/cpu/smt/active)"
  echo "cmdline=$(cat /proc/cmdline)"
  echo
  echo "## Platform power profile / DVFS"
  # On laptops the firmware power profile changes EPP, power limits and the
  # frequency floor.  Numbers from different profiles are not comparable, so
  # record it next to the governor.
  if [ -r /sys/firmware/acpi/platform_profile ]; then
    echo "platform_profile=$(cat /sys/firmware/acpi/platform_profile)"
    echo "platform_profile_choices=$(cat /sys/firmware/acpi/platform_profile_choices 2>/dev/null || echo n/a)"
  else
    echo "platform_profile=n/a"
  fi
  if command -v powerprofilesctl >/dev/null 2>&1; then
    echo "powerprofilesctl=$(powerprofilesctl get 2>/dev/null || echo n/a)"
  else
    echo "powerprofilesctl=missing"
  fi
  if [ -r /sys/class/power_supply/AC/online ] || [ -r /sys/class/power_supply/ACAD/online ]; then
    echo "ac_online=$(cat /sys/class/power_supply/ACAD/online 2>/dev/null || cat /sys/class/power_supply/AC/online 2>/dev/null)"
  fi
  [ -r /sys/devices/system/cpu/cpufreq/boost ] &&
    echo "cpufreq/boost=$(cat /sys/devices/system/cpu/cpufreq/boost)"
  for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
    [ -r "$cpu/cpufreq/energy_performance_preference" ] || continue
    echo "$(basename "$cpu") epp=$(cat "$cpu/cpufreq/energy_performance_preference") cur_khz=$(cat "$cpu/cpufreq/scaling_cur_freq" 2>/dev/null || echo n/a)"
  done
  echo
  echo "## Thermal snapshot"
  for zone in /sys/class/thermal/thermal_zone*; do
    [ -r "$zone/temp" ] || continue
    echo "$(cat "$zone/type" 2>/dev/null || basename "$zone") = $(cat "$zone/temp")mC"
  done
  echo
  echo "## Privileges / limits"
  echo "uid=$(id -u) user=$(id -un)"
  echo "rtprio_limit=$(ulimit -r)"
  echo "perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo n/a)"
  if command -v getcap >/dev/null 2>&1; then
    echo "binary_caps=$(getcap "$ROOT/build-rel/matching_engine" 2>/dev/null || true)"
  fi
  echo
  echo "## Toolchain"
  c++ --version | head -1
  cmake --version | head -1
  if command -v pkg-config >/dev/null 2>&1; then
    echo "liburing=$(pkg-config --modversion liburing 2>/dev/null || echo missing)"
  else
    echo "liburing=pkg-config-missing"
  fi
  for tool in perf strace cpupower taskset python3; do
    if command -v "$tool" >/dev/null 2>&1; then
      echo "$tool=$(command -v "$tool")"
    else
      echo "$tool=missing"
    fi
  done
  echo
  echo "## Host load at capture"
  echo "loadavg=$(cat /proc/loadavg)"
  echo "runnable_snapshot=1s vmstat us/sy/id columns below"
  vmstat 1 3 2>/dev/null | tail -3 || true
  echo "-- top CPU consumers (this is a desktop session, not a dedicated test box) --"
  ps -eo pid,pcpu,stat,comm --sort=-pcpu | head -8
  echo
  echo "## Pin map"
  echo "engine_cpu=${ME_CPU_ENGINE:-2}"
  echo "net_cpu=${ME_CPU_NET:-4} (floating unless ME_PIN_NET=1)"
  echo "client_cpu=${ME_CPU_CLIENT:-6}"
  echo "note=P-cores 0-7 in SMT pairs; E-cores 8-11; avoid siblings and CPU 0"
} | tee "$OUT"
