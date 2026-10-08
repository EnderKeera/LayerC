#!/usr/bin/env bash
# Standalone prototype measurements, with a private store/display/session.
# Usage: measure.sh EXECUTABLE OUTPUT [--client] [--env MEASURED_SETTING]...
# Default settle/idle-CPU windows are 10/60 seconds. The sampler is pre-exec.
# OUTPUT must be absent or empty, so no earlier run's files can be reused.
set -euo pipefail
exe=$(realpath "${1:?executable}")
out=$(realpath -m "${2:?output directory}")
case $(basename "$exe") in
  layere_proto_fltk|layere_proto_fltk_a11y|layere_proto_gtk) ;;
  *) echo 'requires a standalone editor prototype' >&2; exit 64;;
esac
if [[ -e "$out" && ( ! -d "$out" || -n "$(ls -A "$out")" ) ]]; then
  echo 'measurement output must be a new or empty directory' >&2; exit 64
fi
shift 2
live_client=0
extra_env=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --client) live_client=1; shift;;
    --env)
      # Do not permit HOME/XDG/store/display/bus overrides after isolation.
      case "${2:?}" in
        GSK_RENDERER=cairo|GSK_RENDERER=gl|NO_AT_BRIDGE=0|NO_AT_BRIDGE=1|FLTK_SCALING_FACTOR=2|GDK_SCALE=2)
          extra_env+=("$2");;
        *) echo 'unsupported measurement environment setting' >&2; exit 64;;
      esac
      shift 2;;
    *) echo "unknown argument: $1" >&2; exit 64;;
  esac
done
here=$(cd "$(dirname "$0")" && pwd)
source "$here/private_session.sh"
proto_init "$out" 1600x1000x24
unset GDK_SCALE GDK_DPI_SCALE FLTK_SCALING_FACTOR
proto_run bash "$here/measure_session.sh" "$exe" "$out" "$here" "$live_client" "${extra_env[@]}"
