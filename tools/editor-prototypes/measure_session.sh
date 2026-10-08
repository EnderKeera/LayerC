#!/usr/bin/env bash
# Internal to measure.sh: inherits its authenticated display and private bus.
set -euo pipefail
exe=$1 out=$2 here=$3 live_client=$4
shift 4
extra_env=("$@")
fixture=${LAYERC_FIXTURE:-$here/../editor_store_fixture.py}
settle=${SETTLE_SECONDS:-10}
cpu_window=${CPU_SECONDS:-60}
# Refuse accidental direct invocation against an ambient display or bus.
[[ "$(basename "${XDG_RUNTIME_DIR:?}")" == runtime ]] || exit 1
private_root=$(dirname "$XDG_RUNTIME_DIR")
[[ "$(basename "$private_root")" == layerc-editor-prototype.* &&
   "${XAUTHORITY:-}" == "$private_root/Xauthority" && -s "$XAUTHORITY" &&
   -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]] || { echo 'private fixture required' >&2; exit 1; }
[[ "$settle" =~ ^[0-9]+$ && "$cpu_window" =~ ^[0-9]+$ ]] || exit 64
(( settle >= 1 && settle <= 3600 && cpu_window >= 1 && cpu_window <= 3600 )) || exit 64
work=$(mktemp -d -p "$XDG_RUNTIME_DIR" measurement.XXXXXXXX)
# owned.sh supervisors own the window manager, client and sampler; the sampler
# alone owns the application. This shell never signals a numeric PID. It only
# reads /proc after confirming the published PID start time and executable.
source "$here/owned.sh"
app_pid=
app_start=
trap 'owned_cleanup; rm -rf -- "$work"' EXIT

# Monotonic centiseconds (CLOCK_BOOTTIME via /proc/uptime), not wall time.
now_cs() {
  local up
  read -r up _ < /proc/uptime
  printf '%d\n' "$(( 10#${up%.*} * 100 + 10#${up#*.} ))"
}

owned_start wm "$out/openbox.log" -- openbox --sm-disable
for ((i=0; i<100; i++)); do
  xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q "window id" && break
  owned_alive wm || exit 1
  sleep .05
done
xprop -root _NET_SUPPORTING_WM_CHECK | grep -q "window id"
if [[ "$live_client" == 1 ]]; then
  owned_start client "$out/client.log" -- python3 "$here/bridge_client.py"
  for ((i=0; i<100; i++)); do
    grep -q "^client-ready" "$out/client.log" && break
    owned_alive client || exit 1
    sleep .05
  done
  grep -q "^client-ready" "$out/client.log"
fi
require_client() {
  [[ "$live_client" != 1 ]] || owned_alive client || { echo 'accessibility client exited' >&2; return 1; }
}

stat_fields() { # -> fields 3.. of /proc/PID/stat, after the parenthesized comm
  local stat
  stat=$(<"/proc/$app_pid/stat") || return 1
  printf '%s' "${stat##*) }"
}
require_app() { # Abort unless the recorded application is still that process.
  local fields
  owned_alive sampler || { echo 'sampler exited unexpectedly' >&2; return 1; }
  fields=($(stat_fields 2>/dev/null)) || { echo 'application exited unexpectedly' >&2; return 1; }
  [[ "${fields[19]}" == "$app_start" && "$(readlink "/proc/$app_pid/exe")" == "$exe" ]] ||
    { echo 'application identity changed' >&2; return 1; }
}
sample_memory() { # sets uss pss rss swap (KiB); identity-checked on both sides
  local sample
  require_app
  # Every field must be present: an unreadable or partial file is a failure,
  # never a zero-valued sample.
  sample=$(awk '/^Rss:/{r=$2; n++} /^Pss:/{p=$2; n++} /^Private_Clean:/{c=$2; n++}
       /^Private_Dirty:/{d=$2; n++} /^Swap:/{s=$2; n++}
       END{if (n != 5) exit 1; printf "%d %d %d %d\n", c+d, p, r, s}' \
       "/proc/$app_pid/smaps_rollup") || { echo 'incomplete memory sample' >&2; return 1; }
  read -r uss pss rss swap <<<"$sample"
  require_app
}
cpu_ticks() {
  local fields
  require_app
  fields=($(stat_fields))
  echo $((fields[11] + fields[12]))
}
wait_for() { # file pattern seconds; aborts promptly if the owned run ends
  local deadline=$(( $(now_cs) + $3 * 100 ))
  until grep -q "$2" "$1" 2>/dev/null; do
    if (( $(now_cs) >= deadline )); then echo "timeout waiting for $2" >&2; return 1; fi
    owned_alive sampler || { echo "sampler exited while waiting for $2" >&2; return 1; }
    if [[ -n "$app_pid" ]]; then require_app; fi
    sleep 0.1
  done
}
expect_fixture() { # log mode store -> fixture JSON, after checking scenario identity
  python3 - "$1" "$2" "$3" <<'PY'
import json, os, sys
log, mode, store = sys.argv[1:]
lines = [l for l in open(log, errors="replace").read().splitlines() if l.startswith("fixture=")]
if len(lines) != 1:
    raise SystemExit("expected exactly one fixture report")
fixture = json.loads(lines[0][len("fixture="):])
expected = {"empty": (0, 0, 0), "representative": (20, 10, 2)}[mode]
actual = (fixture["profiles"], fixture["cooldowns"], fixture["panels"])
if os.path.realpath(fixture["store"]) != os.path.realpath(store) or actual != expected:
    raise SystemExit(f"fixture mismatch: {fixture} expected {store} {expected}")
print(json.dumps(fixture))
PY
}
monitored_sleep() { # seconds, confirming the owned application and client survive
  local until=$(( $(now_cs) + $1 * 100 ))
  while (( $(now_cs) < until )); do require_app; require_client; sleep 1; done
  require_app
  require_client
}
report_field() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$1" "$2"; }

run_scenario() { # name store-mode cycles
  local name=$1 mode=$2 cycles=$3
  local home="$work/$name"
  mkdir -p "$home/config/layerc/profiles" "$home/data" "$home/run"
  chmod 700 "$home/run"
  if [[ $mode == representative ]]; then
    python3 "$fixture" "$home/config/layerc/profiles" >/dev/null
  fi
  local log="$out/$name.log"
  # Fresh per-run handshake files: an earlier run's report can never satisfy
  # this run, and the report is also bound by nonce and application identity.
  local startup="$home/run/startup.json" pid_file="$home/run/app.pid" nonce
  nonce=$(python3 -c 'import secrets; print(secrets.token_hex(16))')
  # The per-scenario override must name the same store the platform resolves.
  owned_start sampler "$out/$name-sampler.log" -- \
    env HOME="$home" XDG_CONFIG_HOME="$home/config" XDG_DATA_HOME="$home/data" \
      XDG_RUNTIME_DIR="$home/run" LAYERC_STORE_ROOT="$home/config/layerc/profiles" \
      LAYERC_PROTO_CYCLES="$cycles" \
      LAYERC_PROTO_RESUME_FILE="$home/run/resume-cycles" "${extra_env[@]}" \
      python3 "$here/start_process.py" "$exe" "$log" "$pid_file" "$startup" "$settle" "$nonce"
  # Published only after the sampler observed the prototype image itself.
  wait_for "$pid_file" '^[0-9]* [0-9]' 15
  read -r app_pid app_start < "$pid_file"
  require_app
  wait_for "$log" '^ready' 30
  local fixture_json
  fixture_json=$(expect_fixture "$log" "$mode" "$home/config/layerc/profiles")
  if [[ "$live_client" == 1 && "$(basename "$exe")" == layere_proto_fltk_a11y ]]; then
    grep -qx 'a11y=available' "$log" || { echo 'required bridge unavailable' >&2; return 1; }
  fi
  wait_for "$startup" '"passed": true' "$((settle + 5))"
  [[ "$(report_field "$startup" nonce)" == "$nonce" &&
     "$(report_field "$startup" applicationPid)" == "$app_pid" &&
     "$(report_field "$startup" applicationStartTime)" == "$app_start" &&
     "$(report_field "$startup" executable)" == "$exe" ]] ||
    { echo 'startup report is not bound to this run' >&2; return 1; }
  local peak_uss
  peak_uss=$(report_field "$startup" peakUssKiB)
  local json="\"name\": \"$name\", \"fixture\": $fixture_json, \"startupPeakUssKiB\": $peak_uss"
  if [[ $cycles -gt 0 ]]; then
    wait_for "$log" '^warm' 60
    monitored_sleep 4
    sample_memory
    if grep -q '^cycles-done' "$log"; then
      echo "invalid warm sample: cycles already completed" >&2; return 1
    fi
    json+=", \"warm\": {\"ussKiB\": $uss, \"pssKiB\": $pss, \"rssKiB\": $rss, \"swapKiB\": $swap}"
    touch "$home/run/resume-cycles"
    wait_for "$log" '^cycles-done' 180
    grep -qx 'cycles-validated=100' "$log" || { echo 'cycle workload did not render every validation dialog' >&2; return 1; }
    monitored_sleep 5
    sample_memory
    json+=", \"afterCycles\": {\"cycles\": $cycles, \"ussKiB\": $uss, \"pssKiB\": $pss, \"rssKiB\": $rss, \"swapKiB\": $swap}"
  fi
  local hz ticks0 ticks1 start_cs end_cs
  hz=$(getconf CLK_TCK)
  require_client
  ticks0=$(cpu_ticks); start_cs=$(now_cs)
  monitored_sleep "$cpu_window"
  ticks1=$(cpu_ticks); end_cs=$(now_cs)
  sample_memory
  # Divide by the observed monotonic interval, not the requested window.
  local cpu elapsed
  elapsed=$(awk -v c=$((end_cs - start_cs)) 'BEGIN{printf "%.2f", c / 100.0}')
  cpu=$(awk -v t=$((ticks1 - ticks0)) -v hz="$hz" -v c=$((end_cs - start_cs)) \
        'BEGIN{printf "%.3f", 100.0 * t / hz / (c / 100.0)}')
  json+=", \"settled\": {\"ussKiB\": $uss, \"pssKiB\": $pss, \"rssKiB\": $rss, \"swapKiB\": $swap}"
  json+=", \"idleCpuPercentOfOneCore\": $cpu, \"cpuSampleSeconds\": $elapsed"
  require_app
  json+=", \"threads\": $(ls "/proc/$app_pid/task" | wc -l)"
  if command -v import >/dev/null; then
    import -window root "$out/$name.png" 2>/dev/null || true
  fi
  require_app
  # Closing the sampler's control descriptor asks it to stop and reap the app.
  owned_stop sampler
  [[ "$(report_field "$startup" stoppedByHarness)" == True &&
     "$(report_field "$startup" passed)" == True ]] ||
    { echo 'sampler did not complete an orderly stop' >&2; return 1; }
  cp "$startup" "$out/$name-startup.json"
  app_pid=
  app_start=
  printf '{%s}' "$json"
}

# Additional shared libraries relative to layerd's own closure.
deps_json() {
  local base=${LAYERD:-}
  local libs total=0 count=0
  libs=$(ldd "$exe" | awk '/=>/{print $3}' | sort -u)
  local baseline=""
  [[ -n $base && -x $base ]] && baseline=$(ldd "$base" | awk '/=>/{print $3}' | sort -u)
  while read -r lib; do
    [[ -z $lib ]] && continue
    grep -qxF "$lib" <<<"$baseline" && continue
    total=$((total + $(stat -L -c %s "$lib")))
    count=$((count + 1))
  done <<<"$libs"
  printf '{"executableBytes": %d, "additionalSharedLibraries": %d, "additionalSharedLibraryBytes": %d}' \
    "$(stat -c %s "$exe")" "$count" "$total"
}

# Run directly, not in command substitutions: the parent EXIT trap must own
# each supervisor if a scenario fails.
run_scenario idle-empty empty 0 >"$out/idle-empty-memory.json"
run_scenario representative representative 0 >"$out/representative-memory.json"
run_scenario cycles representative 100 >"$out/cycles-memory.json"
python3 - "$exe" "$out" "${extra_env[*]:-}" "$(deps_json)" <<'PY'
import datetime
import json
import platform
from pathlib import Path
import sys
exe, directory, environment, footprint = sys.argv[1:]
out = Path(directory)
report = {
    "executable": exe, "env": environment,
    "recordedAtUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "kernel": platform.release(), "footprint": json.loads(footprint),
    "scenarios": [json.loads((out / f"{name}-memory.json").read_text())
                  for name in ("idle-empty", "representative", "cycles")],
    "method": ("pre-exec-gated 5ms startup samples; synchronized pre-cycle baseline; "
               "idle CPU over the observed monotonic interval"),
}
(out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
PY
