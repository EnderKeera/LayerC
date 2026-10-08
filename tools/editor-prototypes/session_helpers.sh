#!/usr/bin/env bash
# Sourced inside proto_run's private session. Starts the owned window manager
# and prototype; owned.sh stops both when the session shell exits.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/owned.sh"
trap owned_cleanup EXIT
session_start_wm() { # LOG
  owned_start wm "$1" -- openbox --sm-disable
  local i
  for ((i=0; i<100; i++)); do
    xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q "window id" && return 0
    owned_alive wm || { echo 'window manager exited' >&2; return 1; }
    sleep .05
  done
  xprop -root _NET_SUPPORTING_WM_CHECK | grep -q "window id"
}
session_window() { # owned NAME -> the unique visible "LayerC Editor" window
  local i window pid
  pid=$(owned_pid "$1")
  for ((i=0; i<100; i++)); do
    window=$(xdotool search --onlyvisible --pid "$pid" --name "LayerC Editor" 2>/dev/null || true)
    [[ -n "$window" ]] && break
    owned_alive "$1" || { echo "$1 exited before mapping its window" >&2; return 1; }
    sleep .05
  done
  [[ -n "$window" && "$window" != *$'\n'* ]] || { echo 'unique prototype window not found' >&2; return 1; }
  printf '%s\n' "$window"
}
session_join() { # NAME [SECONDS]: wait for a requested close to finish, boundedly
  local i limit=$(( ${2:-5} * 20 ))
  for ((i=0; i<limit; i++)); do
    owned_alive "$1" || { owned_wait "$1"; return; }
    sleep .05
  done
  echo "$1 did not exit after close" >&2
  return 1
}
