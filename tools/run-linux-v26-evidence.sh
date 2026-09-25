#!/usr/bin/env bash
set -Eeuo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/linux-lib.sh"

usage() {
  cat <<'EOF'
Usage: run-linux-v26-evidence.sh <gnome|kde> <native|flatpak> <window-xid|->

Environment:
  LAYERC_BUILD_DIR       Native build directory (default: build-linux)
  LAYERC_EVIDENCE_DIR    Artifact root (default: out/benchmarks/v26-linux)
  LAYERC_MANUAL_RESULT   pass or fail; prompted when unset
  LAYERC_MANUAL_NOTES    Notes saved with the run
  LAYERC_RUN_AUTOMATION  1 runs tools/run-linux-tests.sh before physical checks
  LAYERC_AUTOMATION_SUMMARY  Existing summary.json to bind to this run
  LAYERC_REQUIRE_AUTOMATION  1 rejects physical evidence without a bound summary
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

if [[ ${LAYERC_RUN_AUTOMATION:-0} == 1 && -n ${LAYERC_AUTOMATION_SUMMARY:-} ]]; then
  echo "error: set only one of LAYERC_RUN_AUTOMATION and LAYERC_AUTOMATION_SUMMARY" >&2
  exit 64
fi
automation_summary=${LAYERC_AUTOMATION_SUMMARY:-}
if [[ ${LAYERC_RUN_AUTOMATION:-0} == 1 ]]; then
  automation_dir="$out/automated"
  flatpak_policy=off
  [[ $mode == flatpak ]] && flatpak_policy=required
  bash tools/run-linux-tests.sh --build-dir "$build_dir" \
    --evidence-dir "$automation_dir" --flatpak "$flatpak_policy"
  automation_summary="$automation_dir/summary.json"
fi
if [[ -n $automation_summary ]]; then
  commit=$(git rev-parse HEAD)
  python3 - "$automation_summary" "$commit" "$mode" \
    "${LAYERC_REQUIRE_AUTOMATION:-0}" <<'PY'
import json
import sys
from pathlib import Path

summary = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
required = ["overall", "verify-headless", "verify-x11", "native-headless-smoke", "native-x11-smoke"]
if sys.argv[3] == "flatpak":
    required.append("flatpak")
bad = [name for name in required if summary.get("automatedEvidence", {}).get(name, {}).get("status") != "pass"]
if summary.get("commit") != sys.argv[2]:
    raise SystemExit("automated evidence commit does not match the physical checkout")
if sys.argv[4] == "1" and summary.get("sourceTreeClean") is not True:
    raise SystemExit("release evidence requires a clean automated source tree")
if bad:
    raise SystemExit("automated evidence lacks passing stages: " + ", ".join(bad))
PY
  cp "$automation_summary" "$out/automated-summary.json"
  printf 'status=bound\nsource=%s\n' "$automation_summary" >"$out/automation-link.txt"
elif [[ ${LAYERC_REQUIRE_AUTOMATION:-0} == 1 ]]; then
  echo "error: LAYERC_REQUIRE_AUTOMATION=1 needs automated evidence" >&2
  exit 1
else
  printf 'status=not-bound\n' >"$out/automation-link.txt"
fi
if [[ ${LAYERC_REQUIRE_AUTOMATION:-0} == 1 && -n $(git status --porcelain) ]]; then
  echo "error: release physical evidence requires a clean checkout" >&2
  exit 1
fi

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
run_ctl() { "${ctl[@]}" "$@"; }

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

if "${ctl[@]}" status --json >"$out/existing-daemon-status.json" \
    2>"$out/existing-daemon-error.txt"; then
  echo "error: a daemon is already active; stop it yourself before physical testing" >&2
  exit 1
fi

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

layerc_daemon_pid=
cleanup() { layerc_daemon_cleanup run_ctl; }
trap cleanup EXIT

layerc_daemon_start run_ctl \
  "$out/layerd.log" "$out/status-before.json" \
  50 0.1 "error: daemon did not become ready" \
  "error: the daemon launched by this script exited before readiness" "${daemon[@]}"

runtime_pid=$layerc_daemon_pid
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
case "$manual_result" in pass | fail | pending) ;; *)
  echo "error: manual result must be pass, fail, or pending" >&2
  exit 64
esac
printf 'result=%s\nnotes=%s\n' "$manual_result" "$manual_notes" \
  >"$out/manual-result.txt"

"${ctl[@]}" status --json >"$out/status-after.json"
layerc_daemon_stop run_ctl
trap - EXIT

python3 - "$out" "$stamp" "$desktop" "$mode" "$manual_result" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
summary = {
    "schemaVersion": 1,
    "recordedAtUtc": sys.argv[2],
    "desktop": sys.argv[3],
    "packageMode": sys.argv[4],
    "manualResult": sys.argv[5],
    "automatedEvidence": "bound" if (root / "automated-summary.json").exists() else "not-bound",
    "evidenceKind": "physical-x11",
}
(root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
PY

layerc_write_sha256sums "$out"

echo "Evidence archived in $out"
if [[ $manual_result != pass ]]; then
  echo "error: physical checklist is not passed" >&2
  exit 2
fi
