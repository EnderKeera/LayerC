#!/usr/bin/env bash
set -euo pipefail
if ! command -v gjs >/dev/null 2>&1; then
    printf '%s\n' 'gjs missing; JavaScript fixtures unavailable' >&2
    exit 77
fi
exec gjs -m "$@"
