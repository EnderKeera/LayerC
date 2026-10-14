#!/usr/bin/env bash
set -Eeuo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/linux-lib.sh"

usage() {
  echo "Usage: $0 <headless|x11> <bin-dir|flatpak:app-id> <artifact-dir> [profile-json] [fixture-executable]" >&2
}

if [[ $# -lt 3 || $# -gt 5 ]]; then
  usage
  exit 64
fi

mode=$1
bin_dir=$2
artifact_dir=$3
profile_json=${4:-}
fixture_exe=${5:-}
case "$mode" in headless | x11) ;; *) usage; exit 64 ;; esac
# These modes exercise a controlled X11 fixture or no display. Never connect
# to an inherited Wayland socket (including an absolute WSLg socket path).
unset WAYLAND_DISPLAY WAYLAND_SOCKET
export LAYERC_DISPLAY_BACKEND=x11

flatpak_app=
installed_location=
if [[ $bin_dir == flatpak:* ]]; then
  flatpak_app=${bin_dir#flatpak:}
  [[ -n $flatpak_app ]] || { usage; exit 64; }
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
for program in layerd layerctl layercal; do
  installed_bin="$bin_dir/$program"
  [[ -z $flatpak_app ]] || installed_bin="$installed_location/files/bin/$program"
  [[ -x $installed_bin ]] || {
    echo "error: missing installed executable $installed_bin" >&2
    exit 66
  }
done
if [[ $mode == x11 ]]; then
  [[ -n ${DISPLAY:-} ]] || {
    echo "error: x11 smoke requires DISPLAY" >&2
    exit 77
  }
  [[ -x $fixture_exe ]] || {
    echo "error: x11 smoke requires a controlled fixture executable" >&2
    exit 66
  }
else
  unset DISPLAY
fi

mkdir -p "$artifact_dir"
smoke_root=$(mktemp -d "${TMPDIR:-/tmp}/layerc-linux-smoke.XXXXXX")
export XDG_CONFIG_HOME="$smoke_root/config"
export XDG_DATA_HOME="$smoke_root/data"
export XDG_CACHE_HOME="$smoke_root/cache"
export XDG_STATE_HOME="$smoke_root/state"
export XDG_RUNTIME_DIR="$smoke_root/runtime"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" \
  "$XDG_STATE_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$smoke_root" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" \
  "$XDG_CACHE_HOME" "$XDG_STATE_HOME" "$XDG_RUNTIME_DIR"
if [[ -n $flatpak_app ]]; then
  export HOME="$smoke_root/home"
  mkdir -p "$HOME"
  chmod 700 "$HOME"
fi

layerc_daemon_pid=
fixture_pid=
cleanup() {
  layerc_daemon_cleanup run_ctl
  if [[ -n $fixture_pid ]] && kill -0 "$fixture_pid" 2>/dev/null; then
    kill "$fixture_pid" 2>/dev/null || true
    wait "$fixture_pid" 2>/dev/null || true
  fi
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

daemon=("$bin_dir/layerd")
if [[ -n $flatpak_app ]]; then
  daemon=(flatpak run --user --command=layerd "$flatpak_app")
fi
layerc_daemon_start run_ctl \
  "$artifact_dir/layerd.log" "$artifact_dir/status-before.json" \
  100 0.05 "error: installed daemon did not become ready" "" "${daemon[@]}"

{
  ps -p "$layerc_daemon_pid" -o pid=,etimes=,pcpu=,rss=,vsz=
  if [[ -d /proc/$layerc_daemon_pid/fd ]]; then
    printf 'fd=%s\n' "$(find "/proc/$layerc_daemon_pid/fd" -mindepth 1 -maxdepth 1 | wc -l)"
  fi
} >"$artifact_dir/resources.txt"

if [[ $mode == x11 ]]; then
  "$fixture_exe" >"$artifact_dir/fixture-window-id.txt" \
    2>"$artifact_dir/fixture.log" &
  fixture_pid=$!
  for _ in {1..100}; do
    [[ -s $artifact_dir/fixture-window-id.txt ]] && break
    kill -0 "$fixture_pid" 2>/dev/null || break
    sleep 0.05
  done
  [[ -s $artifact_dir/fixture-window-id.txt ]] || {
    echo "error: controlled X11 fixture did not become ready" >&2
    exit 1
  }
  read -r window_id <"$artifact_dir/fixture-window-id.txt"
  [[ $window_id =~ ^[0-9]+$ ]] || {
    echo "error: fixture did not report a decimal X11 window id" >&2
    exit 1
  }
  for quality in low balanced; do
    run_program layercal snapshot "$window_id" --quality "$quality" \
      >"$artifact_dir/capture-$quality.ppm" \
      2>"$artifact_dir/capture-$quality.log"
    run_program layercal snapshot "$window_id" --quality "$quality" --json \
      >"$artifact_dir/capture-$quality.json" \
      2>>"$artifact_dir/capture-$quality.log"
  done
  python3 - "$artifact_dir" "$window_id" <<'PY'
import json
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
window = sys.argv[2]
for quality in ("low", "balanced"):
    pixels = (root / f"capture-{quality}.ppm").read_bytes()
    header = re.match(rb"P6\n([0-9]+) ([0-9]+)\n255\n", pixels)
    metadata = json.loads((root / f"capture-{quality}.json").read_text())
    if not header or len(pixels) <= header.end():
        raise SystemExit(f"{quality}: missing P6 capture pixels")
    if (metadata.get("window") != window or
        metadata.get("quality") != quality or
        metadata.get("availability") != "available" or
        metadata.get("width") != int(header[1]) or
        metadata.get("height") != int(header[2]) or
        metadata.get("snapshot", {}).get("bytes") != len(pixels)):
        raise SystemExit(f"{quality}: capture metadata does not match fixture pixels")
PY
fi

run_program layerctl status --json >"$artifact_dir/status-after.json"
layerc_daemon_stop run_ctl

if find "$XDG_RUNTIME_DIR" -type s -print -quit | grep -q .; then
  echo "error: daemon left a control socket after clean shutdown" >&2
  exit 1
fi

printf 'result=pass\nmode=%s\n' "$mode" >"$artifact_dir/result.txt"
