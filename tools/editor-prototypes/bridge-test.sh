#!/usr/bin/env bash
# Prototype-only semantic probe; never access an ambient store/display/bus.
# Usage: bridge-test.sh ABSOLUTE_EXECUTABLE OUTPUT_DIRECTORY
set -euo pipefail
exe=$(realpath "${1:?executable}")
out=$(realpath -m "${2:?output directory}")
here=$(cd "$(dirname "$0")" && pwd)
[[ $(basename "$exe") == layere_proto_fltk_a11y ]] || exit 64
source "$here/private_session.sh"
proto_init "$out" 1920x1200x24
python3 "$here/../editor_store_fixture.py" "$LAYERC_STORE_ROOT"
proto_run bash -euo pipefail -c '
  source "$3/session_helpers.sh"
  session_start_wm "$2/openbox.log"
  owned_start app "$2/prototype.log" -- env G_DEBUG=fatal-warnings "$1"
  python3 "$3/bridge_test.py" "$2" "$LAYERC_STORE_ROOT"
  session_join app
  owned_start disabled "$2/disabled.log" -- env G_DEBUG=fatal-warnings NO_AT_BRIDGE=1 "$1"
  for ((i=0; i<100; i++)); do
    grep -q "^a11y=unavailable" "$2/disabled.log" && break
    owned_alive disabled || exit 1
    sleep .05
  done
  grep -q "^a11y=unavailable" "$2/disabled.log"
  import -window root "$2/disabled.png"
  xdotool key --clearmodifiers alt+F4
  session_join disabled
' bash "$exe" "$out" "$here" >"$out/session.log" 2>&1
echo "Bridge probe passed"
