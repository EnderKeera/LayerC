#!/usr/bin/env bash
# Sourced by run-linux-tests.sh after CTest registration is recorded.
# Uses its run_logged/record_stage functions and build/source/evidence paths.
# Protocol/connection failure fixtures live in the base gates, not in the
# compositor label. Required backend coverage must not silently lose them, but
# must leave unrelated optional tooling (PowerShell) optional.
verify_base_gate() {
  local label=$1
  local strict=()
  [[ $wayland_policy != required ]] ||
    strict=(--require-optional-prefix layerc_linux_wayland_)
  run_logged "verify-$label" python3 "$source_dir/tools/linux_test_report.py" \
    --registration "$evidence_dir/ctest-registration.json" \
    --junit "$evidence_dir/ctest-$label.xml" --gate "linux-$label" \
    --report "$evidence_dir/ctest-$label-summary.json" "${strict[@]}"
}

run_backend_gate() {
  local label=$1 reason= tool rc=0
  shift
  local report="$evidence_dir/ctest-$label-summary.json"
  local strict=()
  if [[ $wayland_policy == off ]]; then
    record_stage "$label" not-run 'disabled by policy; no V2.7 backend coverage was claimed'
    return
  fi
  # Use the policy, not just labels: losing a label must still fail verification.
  python3 - "$source_dir/tools" "$evidence_dir/ctest-registration.json" "$label" <<'PY' || rc=$?
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from linux_test_report import load_gate, registered_names
gate = load_gate(Path(sys.argv[1]) / "test-gates.json", "linux-" + sys.argv[3])
registered = registered_names(Path(sys.argv[2]))
sys.exit(0 if registered & (gate.required | gate.optional) else 3)
PY
  if ((rc == 3)); then
    reason="the build registered no linux-$label gate suites"
  elif ((rc)); then
    record_stage "$label" fail 'could not inspect CTest registration'
    return "$rc"
  fi
  for tool in "$@"; do
    if ! command -v "$tool" >/dev/null; then
      reason="${reason:+$reason; }missing required harness: $tool"
    fi
  done
  if [[ -n $reason ]]; then
    if [[ $wayland_policy == required ]]; then
      record_stage "$label" fail "$reason"
      echo "error: required $label coverage unavailable: $reason" >&2
      return 1
    fi
    record_stage "$label" unavailable "$reason"
    return
  fi
  [[ $wayland_policy != required ]] || strict=(--require-all)
  # Each suite owns its compositor/bus fixture. Clear the caller's desktop and
  # backend/input opt-in before running those fixtures.
  run_logged "ctest-$label" env -u DISPLAY -u WAYLAND_DISPLAY -u WAYLAND_SOCKET \
    -u DBUS_SESSION_BUS_ADDRESS -u LAYERC_DISPLAY_BACKEND -u LAYERC_INPUT \
    ctest --test-dir "$build_dir" -L "^$label$" --output-on-failure --no-tests=error \
    --output-junit "$evidence_dir/ctest-$label.xml" || return $?
  run_logged "verify-$label" python3 "$source_dir/tools/linux_test_report.py" \
    --registration "$evidence_dir/ctest-registration.json" \
    --junit "$evidence_dir/ctest-$label.xml" --gate "linux-$label" \
    --report "$report" "${strict[@]}" || return $?
  if python3 - "$report" <<'PY'
import json, sys
sys.exit(0 if json.load(open(sys.argv[1]))["skipped"] else 1)
PY
  then
    record_stage "$label" unavailable 'allowed harness skips; see CTest summary; not full coverage'
  else
    record_stage "$label" pass
  fi
}

run_backend_gates() {
  run_backend_gate wayland sway swaymsg || return $?
  run_backend_gate dbus dbus-run-session || return $?
  run_backend_gate gjs gjs dbus-run-session
}
