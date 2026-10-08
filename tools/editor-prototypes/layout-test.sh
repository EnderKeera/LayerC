#!/usr/bin/env bash
# Usage: layout-test.sh ABSOLUTE_PROTOTYPE OUTPUT_DIRECTORY 1|2
set -euo pipefail
exe=$(realpath "${1:?executable}")
out=$(realpath -m "${2:?output directory}")
scale=${3:?scale}
[[ "$scale" == 1 || "$scale" == 2 ]] || { echo 'scale must be 1 or 2' >&2; exit 64; }
case $(basename "$exe") in
  layere_proto_fltk_a11y|layere_proto_gtk) ;;
  *) echo 'layout probe requires an accessible standalone prototype' >&2; exit 64 ;;
esac
here=$(cd "$(dirname "$0")" && pwd)
source "$here/private_session.sh"
proto_init "$out" 2880x1800x24
python3 "$here/../editor_store_fixture.py" "$LAYERC_STORE_ROOT"
unset FLTK_SCALING_FACTOR GDK_SCALE GDK_DPI_SCALE
proto_run bash -euo pipefail -c '
  source "$3/session_helpers.sh"
  session_start_wm "$2/openbox.log"
  owned_start app "$2/prototype.log" -- \
    env G_DEBUG=fatal-warnings FLTK_SCALING_FACTOR="$4" GDK_SCALE="$4" "$1"
  window=$(session_window app)
  python3 "$3/layout_test.py" "$2" "$(basename "$1")" "$window" "$4" "$LAYERC_STORE_ROOT"
  session_join app
' bash "$exe" "$out" "$here" "$scale" >"$out/session.log" 2>&1
