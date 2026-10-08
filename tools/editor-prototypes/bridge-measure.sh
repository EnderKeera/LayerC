#!/usr/bin/env bash
# The subscribed client starts only after private display/store/bus isolation.
# Defaults retain measure.sh's 10/60-second settle/idle-CPU windows.
set -euo pipefail
exe=${1:?executable}
out=${2:?output directory}
here=$(cd "$(dirname "$0")" && pwd)
exec bash "$here/measure.sh" "$exe" "$out" --client --env NO_AT_BRIDGE=0
