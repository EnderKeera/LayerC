#!/usr/bin/env bash
# Installed Linux LayerE smoke: start the installed editor beside the installed
# daemon on the caller's (disposable) X display, confirm its main window maps,
# record idle memory/CPU against the approved targets, and quit it through its
# own Ctrl+Q command. Private XDG roots keep the user's store and socket out of
# reach; the caller owns the display (xvfb-run in the automated gates).
set -Eeuo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/linux-lib.sh"

usage() {
  echo "Usage: $0 <bin-dir|flatpak:app-id> <artifact-dir> [profile-json] [idle-seconds]" >&2
}

if [[ $# -lt 2 || $# -gt 4 ]]; then
  usage
  exit 64
fi
bin_dir=$1
artifact_dir=$2
profile_json=${3:-}
idle_seconds=${4:-20}
[[ $idle_seconds =~ ^[1-9][0-9]*$ ]] || { usage; exit 64; }
# Approved idle targets (docs/LinuxEditorPlan.md section 7).
uss_limit_mib=20
cpu_limit_percent=0.2

[[ -n ${DISPLAY:-} ]] || {
  echo "error: the editor smoke requires DISPLAY" >&2
  exit 77
}
for tool in xdotool python3; do
  command -v "$tool" >/dev/null || {
    echo "error: the editor smoke requires $tool" >&2
    exit 69
  }
done

flatpak_app=
installed_location=
if [[ $bin_dir == flatpak:* ]]; then
  flatpak_app=${bin_dir#flatpak:}
  [[ -n $flatpak_app ]] || { usage; exit 64; }
  # Resolve the caller's user installation before HOME/XDG are redirected.
  export FLATPAK_USER_DIR=${FLATPAK_USER_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/flatpak}
  installed_location=$(flatpak info --user --show-location "$flatpak_app")
fi
run_program() {
  local program=$1
  shift
  if [[ -n $flatpak_app ]]; then
    flatpak run --user --command="$program" "$flatpak_app" "$@"
  else
    "$bin_dir/$program" "$@"
  fi
}
run_ctl() { run_program layerctl "$@"; }
for program in layerd layerctl layere; do
  installed_bin="$bin_dir/$program"
  [[ -z $flatpak_app ]] || installed_bin="$installed_location/files/bin/$program"
  [[ -x $installed_bin ]] || {
    echo "error: missing installed executable $installed_bin" >&2
    exit 66
  }
done

mkdir -p "$artifact_dir"
smoke_root=$(mktemp -d "${TMPDIR:-/tmp}/layerc-editor-smoke.XXXXXX")
export XDG_CONFIG_HOME="$smoke_root/config"
export XDG_DATA_HOME="$smoke_root/data"
export XDG_CACHE_HOME="$smoke_root/cache"
export XDG_STATE_HOME="$smoke_root/state"
export XDG_RUNTIME_DIR="$smoke_root/runtime"
export HOME="$smoke_root/home"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" \
  "$XDG_STATE_HOME" "$XDG_RUNTIME_DIR" "$HOME"
chmod 700 "$smoke_root" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" \
  "$XDG_CACHE_HOME" "$XDG_STATE_HOME" "$XDG_RUNTIME_DIR" "$HOME"

# shellcheck disable=SC2034 # set and read by the linux-lib.sh daemon helpers
layerc_daemon_pid=
editor_pid=
cleanup() {
  if [[ -n $editor_pid ]] && kill -0 "$editor_pid" 2>/dev/null; then
    kill "$editor_pid" 2>/dev/null || true
    wait "$editor_pid" 2>/dev/null || true
  fi
  layerc_daemon_cleanup run_ctl
  rm -rf -- "$smoke_root"
}
trap cleanup EXIT

if [[ -n $profile_json ]]; then
  [[ -f $profile_json ]] || {
    echo "error: profile fixture not found: $profile_json" >&2
    exit 66
  }
  run_program layerctl profile import-json <"$profile_json" \
    >"$artifact_dir/profile-import.log" 2>&1
fi

run_program layere --version >"$artifact_dir/layere-version.txt"

daemon=("$bin_dir/layerd")
[[ -z $flatpak_app ]] || daemon=(flatpak run --user --command=layerd "$flatpak_app")
layerc_daemon_start run_ctl \
  "$artifact_dir/layerd.log" "$artifact_dir/status-before.json" \
  100 0.05 "error: installed daemon did not become ready" "" "${daemon[@]}"

# Background the program itself, not a shell function, so cleanup's kill
# reaches the editor (or its flatpak run wrapper) rather than a subshell.
editor=("$bin_dir/layere")
[[ -z $flatpak_app ]] || editor=(flatpak run --user --command=layere "$flatpak_app")
# Only a window that appears after launch is ours; never touch an earlier one.
earlier_windows=$(xdotool search --onlyvisible --name '^LayerE' 2>/dev/null || true)
"${editor[@]}" >"$artifact_dir/layere.log" 2>&1 &
editor_pid=$!

# The window title is the editor's own; any earlier LayerE window would be
# another process on a display this gate does not own.
window=
deadline=$((SECONDS + 15))
while ((SECONDS < deadline)); do
  kill -0 "$editor_pid" 2>/dev/null || break
  window=$(xdotool search --onlyvisible --name '^LayerE' 2>/dev/null |
    grep -vxF -e "${earlier_windows:-none}" | head -n1 || true)
  [[ -n $window ]] && break
  sleep 0.1
done
[[ -n $window ]] || {
  echo "error: the installed editor did not map its main window" >&2
  exit 1
}
xdotool getwindowname "$window" >"$artifact_dir/window-title.txt"

# Flatpak wraps the editor in bwrap; measure the layere process itself.
python3 - "$editor_pid" "$idle_seconds" "$uss_limit_mib" "$cpu_limit_percent" \
  "$artifact_dir/resources.json" <<'PY'
import json
import os
import sys
import time
from pathlib import Path

root_pid, idle, uss_limit, cpu_limit, report = sys.argv[1:]
idle, uss_limit, cpu_limit = float(idle), float(uss_limit), float(cpu_limit)

def children(pid):
    found = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
        except OSError:
            continue
        if int(stat.rsplit(")", 1)[1].split()[1]) == pid:
            found.append(int(entry.name))
    return found

def editor(pid):
    queue = [pid]
    while queue:
        current = queue.pop(0)
        try:
            if Path(f"/proc/{current}/comm").read_text().strip() == "layere":
                return current
        except OSError:
            pass
        queue.extend(children(current))
    raise SystemExit("error: no layere process under the started editor")

def ticks(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return int(fields[11]) + int(fields[12])

def memory(pid):
    values = {}
    for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines()[1:]:
        key, value = line.split(":", 1)
        values[key] = int(value.split()[0])
    return {
        "ussMiB": (values["Private_Clean"] + values["Private_Dirty"]) / 1024,
        "pssMiB": values["Pss"] / 1024,
        "rssMiB": values["Rss"] / 1024,
        "swapMiB": values.get("Swap", 0) / 1024,
    }

pid = editor(int(root_pid))
hz = os.sysconf("SC_CLK_TCK")
# Let startup settle before the idle window, then sample across it.
time.sleep(2)
start_ticks, start = ticks(pid), time.monotonic()
time.sleep(idle)
end_ticks, elapsed = ticks(pid), time.monotonic() - start
result = memory(pid)
result.update(
    editorPid=pid,
    idleSeconds=round(elapsed, 3),
    cpuPercent=round(100.0 * (end_ticks - start_ticks) / hz / elapsed, 4),
    targets={"ussMiB": uss_limit, "cpuPercent": cpu_limit},
    definitions={
        "ussMiB": "Private_Clean + Private_Dirty from /proc/<pid>/smaps_rollup",
        "cpuPercent": "utime + stime ticks over the idle window",
    },
)
result["result"] = (
    "pass" if result["ussMiB"] <= uss_limit and result["cpuPercent"] <= cpu_limit else "fail"
)
Path(report).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps(result))
if result["result"] != "pass":
    raise SystemExit("error: installed editor exceeded its idle resource target")
PY

# Quit through the editor's own File/Quit shortcut; an unmodified document
# closes without a prompt and the process must exit cleanly.
xdotool windowfocus --sync "$window"
xdotool key --clearmodifiers ctrl+q
editor_rc=
deadline=$((SECONDS + 10))
while ((SECONDS < deadline)); do
  if ! kill -0 "$editor_pid" 2>/dev/null; then
    set +e
    wait "$editor_pid"
    editor_rc=$?
    set -e
    break
  fi
  sleep 0.1
done
[[ -n $editor_rc ]] || {
  echo "error: the editor did not quit on Ctrl+Q" >&2
  exit 1
}
editor_pid=
[[ $editor_rc -eq 0 ]] || {
  echo "error: the editor exited with status $editor_rc" >&2
  exit 1
}

run_program layerctl status --json >"$artifact_dir/status-after.json"
layerc_daemon_stop run_ctl

printf 'result=pass\nwindow=%s\n' "$(cat "$artifact_dir/window-title.txt")" \
  >"$artifact_dir/result.txt"
