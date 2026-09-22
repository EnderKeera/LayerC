#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat <<'EOF'
Usage: run-linux-v26-evidence.sh <gnome|kde> <native|flatpak> <window-xid|->

Environment:
  LAYERC_BUILD_DIR       Native build directory (default: build-linux)
  LAYERC_EVIDENCE_DIR    Artifact root (default: out/benchmarks/v26-linux)
  LAYERC_MANUAL_RESULT   pass or fail; prompted when unset
  LAYERC_MANUAL_NOTES    Notes saved with the run
EOF
}

if [[ $# -ne 3 ]]; then
  usage >&2
  exit 64
fi

desktop=$1
mode=$2
window_id=$3
case "$desktop" in gnome | kde) ;; *)
  usage >&2
  exit 64
  ;;
esac
case "$mode" in native | flatpak) ;; *)
  usage >&2
  exit 64
  ;;
esac

if [[ ${XDG_SESSION_TYPE:-} != x11 || -z ${DISPLAY:-} ]]; then
  echo "error: run this from a physical X11 session" >&2
  exit 77
fi

build_dir=${LAYERC_BUILD_DIR:-build-linux}
evidence_root=${LAYERC_EVIDENCE_DIR:-out/benchmarks/v26-linux}
stamp=$(date -u +%Y%m%dT%H%M%SZ)
out="$evidence_root/$stamp-$desktop-$mode"
mkdir -p "$out"
exec > >(tee "$out/run.log") 2>&1

app_id=io.github.EnderKeera.LayerC
if [[ $mode == native ]]; then
  daemon=("$build_dir/layerd")
  ctl=("$build_dir/layerctl")
  cal=("$build_dir/layercal")
  for command in "${daemon[0]}" "${ctl[0]}" "${cal[0]}"; do
    [[ -x $command ]] || {
      echo "error: missing $command" >&2
      exit 66
    }
  done
else
  command -v flatpak >/dev/null
  flatpak info "$app_id" >"$out/flatpak-info.txt"
  daemon=(flatpak run "$app_id")
  ctl=(flatpak run --command=layerctl "$app_id")
  cal=(flatpak run --command=layercal "$app_id")
fi

{
  printf 'recordedAtUtc=%s\n' "$stamp"
  printf 'desktop=%s\nmode=%s\n' "$desktop" "$mode"
  printf 'XDG_SESSION_TYPE=%s\nDISPLAY=%s\n' "$XDG_SESSION_TYPE" "$DISPLAY"
  printf 'XDG_CURRENT_DESKTOP=%s\n' "${XDG_CURRENT_DESKTOP:-}"
  uname -a
} >"$out/environment.txt"
git rev-parse HEAD >"$out/git-head.txt" 2>/dev/null || true
git status --short >"$out/git-status.txt" 2>/dev/null || true
command -v xrandr >/dev/null && xrandr --current >"$out/xrandr.txt"
command -v xdpyinfo >/dev/null && xdpyinfo >"$out/xdpyinfo.txt"

cat >"$out/manual-checklist.txt" <<'EOF'
Confirm all items before entering pass:
- Passive keyboard and mouse input trigger the expected cooldowns.
- Hierarchy hotplug is reported; unavailable controllers have a stable reason.
- Executable/title/class matching selects only the intended window.
- Overlay remains visible, correctly scaled, and click-through at tested DPI.
- Numeric visual and template rules each drive a cooldown/automation transition.
- Composite capture and direct fallback recover after resize/destroy/black frames.
- Low/Balanced CPU, RSS, descriptor, session, asset, and cache counts stay bounded.
- Closing the session returns capture/assets/cache to zero with no idle polling.
EOF

"${daemon[@]}" >"$out/layerd.log" 2>&1 &
daemon_pid=$!
cleanup() {
  if kill -0 "$daemon_pid" 2>/dev/null; then
    "${ctl[@]}" stop >/dev/null 2>&1 || kill "$daemon_pid" 2>/dev/null || true
    wait "$daemon_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

ready=false
for _ in {1..50}; do
  if "${ctl[@]}" status --json >"$out/status-before.json" 2>/dev/null; then
    ready=true
    break
  fi
  sleep 0.1
done
if [[ $ready != true ]]; then
  echo "error: daemon did not become ready" >&2
  exit 1
fi

runtime_pid=$daemon_pid
if [[ $mode == flatpak ]]; then
  flatpak_pid=$(flatpak ps --columns=pid,application 2>/dev/null |
    awk -v app="$app_id" '$2 == app { print $1; exit }')
  [[ -n ${flatpak_pid:-} ]] && runtime_pid=$flatpak_pid
fi
for _ in {1..10}; do
  ps -p "$runtime_pid" -o pid=,etimes=,pcpu=,rss=,vsz= >>"$out/resources.txt" || true
  if [[ -d /proc/$runtime_pid/fd ]]; then
    printf 'fd=%s\n' "$(find "/proc/$runtime_pid/fd" -mindepth 1 -maxdepth 1 | wc -l)" \
      >>"$out/resources.txt"
  fi
  sleep 1
done

if [[ $window_id != - ]]; then
  "${cal[@]}" snapshot "$window_id" --quality low >"$out/low.ppm"
  "${cal[@]}" snapshot "$window_id" --quality low --json >"$out/low.json"
  "${cal[@]}" snapshot "$window_id" --quality balanced >"$out/balanced.ppm"
  "${cal[@]}" snapshot "$window_id" --quality balanced --json \
    >"$out/balanced.json"
fi

cat "$out/manual-checklist.txt"
manual_result=${LAYERC_MANUAL_RESULT:-}
manual_notes=${LAYERC_MANUAL_NOTES:-}
if [[ -z $manual_result && -t 0 ]]; then
  read -r -p 'Manual checks result [pass/fail]: ' manual_result
  read -r -p 'Notes: ' manual_notes
fi
manual_result=${manual_result:-pending}
printf 'result=%s\nnotes=%s\n' "$manual_result" "$manual_notes" \
  >"$out/manual-result.txt"

"${ctl[@]}" status --json >"$out/status-after.json"
"${ctl[@]}" stop
wait "$daemon_pid"
trap - EXIT

checksums=$(mktemp)
(
  cd "$out"
  find . -type f ! -name SHA256SUMS -print0 | sort -z |
    xargs -0 sha256sum
) >"$checksums"
mv "$checksums" "$out/SHA256SUMS"

echo "Evidence archived in $out"
if [[ $manual_result != pass ]]; then
  echo "error: physical checklist is not passed" >&2
  exit 2
fi
