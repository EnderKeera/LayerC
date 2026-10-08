#!/usr/bin/env bash
# Prototype-only tree, keyboard/save/menu screenshots and 2x rendering.
# Usage: interaction.sh ABSOLUTE_PROTOTYPE OUTPUT SCALE_ENV [--env GSK_RENDERER=cairo]
set -euo pipefail
exe=$(realpath "${1:?executable}")
out=$(realpath -m "${2:?output directory}")
scale_env=${3:?FLTK_SCALING_FACTOR=2 or GDK_SCALE=2}
case $(basename "$exe") in
  layere_proto_fltk|layere_proto_fltk_a11y|layere_proto_gtk) ;;
  *) echo 'requires a standalone editor prototype' >&2; exit 64 ;;
esac
case "$scale_env" in FLTK_SCALING_FACTOR=2|GDK_SCALE=2) ;; *) exit 64 ;; esac
shift 3
extra_env=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --env)
      # Never permit overrides of the private display, bus, home or store.
      case "${2:?}" in GSK_RENDERER=cairo|GSK_RENDERER=gl) extra_env+=("$2");;
        *) echo 'only a measured GTK renderer override is supported' >&2; exit 64;; esac
      shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 64 ;;
  esac
done
here=$(cd "$(dirname "$0")" && pwd)
source "$here/private_session.sh"
unset GDK_SCALE GDK_DPI_SCALE FLTK_SCALING_FACTOR
for phase in normal hidpi; do
  mkdir -p "$out/$phase"
  # Each scale gets a fresh server as well as a fresh store/bus. Reusing the
  # server retains stale EWMH properties from the previous window manager.
  proto_init "$out/$phase" 2880x1800x24
  python3 "$here/../editor_store_fixture.py" "$LAYERC_STORE_ROOT"
  scaling=()
  [[ "$phase" == normal ]] || scaling+=("$scale_env")
  proto_run bash -euo pipefail -c '
    exe=$1 out=$2 here=$3 phase=$4
    shift 4
    source "$here/session_helpers.sh"
    session_start_wm "$out/openbox.log"
    owned_start app "$out/prototype.log" -- env G_DEBUG=fatal-warnings "$@" "$exe"
    window=$(session_window app)
    xdotool windowactivate --sync "$window"
    import -window root "$out/01-start.png"
    timeout 20 python3 "$here/a11y_dump.py" "$(basename "$exe")" >"$out/a11y.json" 2>"$out/a11y.err"
    if [[ "$phase" == normal ]]; then
      for step in 1 2 3 4 5 6; do xdotool key --clearmodifiers Tab; done
      import -window root "$out/02-after-6-tabs.png"
      xdotool key --clearmodifiers alt+s
      sleep .25
      import -window root "$out/03-alt-s-save.png"
      xdotool mousemove --window "$window" 100 120 click 3
      sleep .25
      import -window root "$out/04-context-menu.png"
      xdotool key Escape
    fi
    # The owned native main window receives WM_DELETE_WINDOW, not an X surface
    # destroy or a possibly intercepted WM hotkey. This is a protocol-close test.
    sleep .25
    timeout 10 python3 "$here/close_window.py" "$window" "$(owned_pid app)"
    session_join app
  ' bash "$exe" "$out/$phase" "$here" "$phase" "${extra_env[@]}" "${scaling[@]}" >"$out/$phase/session.log" 2>&1
  proto_cleanup
done
python3 - "$out" <<'PY'
import json, pathlib, sys
out = pathlib.Path(sys.argv[1])
summary = {}
for phase in ('normal', 'hidpi'):
    data = json.loads((out/phase/'a11y.json').read_text())
    summary[phase] = [{k:v for k,v in app.items() if k != 'tree'} for app in data['applications']]
(out/'a11y-summary.json').write_text(json.dumps(summary, indent=2)+'\n')
print(json.dumps(summary, indent=2))
PY
