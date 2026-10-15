#!/usr/bin/env bash
# Isolated local IPC only: never connect tests to the caller's session bus.
set -euo pipefail
if ! command -v dbus-run-session >/dev/null 2>&1; then
    printf '%s\n' 'dbus-run-session missing; bus fixtures unavailable' >&2
    exit 77
fi
exec dbus-run-session -- "$@"
