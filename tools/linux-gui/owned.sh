#!/usr/bin/env bash
# Sourced GUI harness process ownership (bash 4.3+). Every long-lived
# helper runs under owned.py, which keeps it unreaped and stops it when its
# control FIFO is written or closed. Scripts never signal a numeric PID; when
# the shell exits for any reason, the kernel closes its control descriptors.
owned_root=
owned_names=()
declare -A owned_fd=() owned_supervisor=()
owned_here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

owned_init() {
  [[ -n "$owned_root" ]] && return 0
  # Outside any disposable fixture, so a fixture cannot reach the FIFOs.
  owned_root=$(mktemp -d -t layerc-proto-owned.XXXXXXXX)
}
owned_start() { # NAME LOG|- [--group] -- COMMAND [ARG]...  ("-" inherits output)
  local name=$1 log=$2 fd options=()
  shift 2
  while [[ "$1" != -- ]]; do options+=("$1"); shift; done
  shift
  owned_init
  local state="$owned_root/$name"
  [[ -z "${owned_fd[$name]:-}" ]] || { echo "owned $name already running" >&2; return 1; }
  rm -f -- "$state.ctl" "$state.pid" "$state.status"
  mkfifo -m 600 "$state.ctl"
  # Open the shell's end first (read-write never blocks on Linux). The
  # background child opens the read end as its stdin while it still holds an
  # inherited write end, so that open cannot block; the supervisor then
  # closes the inherited copy, leaving this shell as the only writer.
  exec {fd}<>"$state.ctl"
  if [[ "$log" == - ]]; then
    python3 "$owned_here/owned.py" "${options[@]}" - "$state" -- "$@" <"$state.ctl" &
  else
    python3 "$owned_here/owned.py" "${options[@]}" - "$state" -- "$@" <"$state.ctl" >"$log" 2>&1 &
  fi
  owned_supervisor[$name]=$!
  owned_fd[$name]=$fd
  owned_names+=("$name")
  local i
  for ((i=0; i<500; i++)); do
    [[ -s "$state.pid" ]] && return 0
    [[ -e "$state.status" ]] && break
    sleep .01
  done
  echo "owned $name did not start" >&2
  return 1
}
owned_pid() { local pid; read -r pid < "$owned_root/$1.pid"; printf '%s\n' "$pid"; }
owned_alive() { [[ -n "${owned_fd[$1]:-}" && ! -e "$owned_root/$1.status" ]]; }
owned_release() { # NAME: close the control descriptor and join the supervisor
  local name=$1 fd=${owned_fd[$1]:-} result=0
  [[ -n "$fd" ]] || return 0
  exec {fd}>&-
  unset 'owned_fd[$name]'
  wait "${owned_supervisor[$name]}" || result=$?
  unset 'owned_supervisor[$name]'
  return "$result"
}
owned_stop() { owned_release "$1"; }  # Closing the FIFO requests a bounded stop.
owned_wait() { # NAME: join a natural exit, keeping ownership until it happens
  local name=$1 result=0
  wait "${owned_supervisor[$name]}" || result=$?
  local fd=${owned_fd[$name]:-}
  if [[ -n "$fd" ]]; then exec {fd}>&-; unset 'owned_fd[$name]'; fi
  unset 'owned_supervisor[$name]'
  return "$result"
}
owned_cleanup() { # Stop in reverse start order, then remove the FIFOs.
  local i
  for ((i=${#owned_names[@]}-1; i>=0; i--)); do owned_release "${owned_names[$i]}" || true; done
  owned_names=()
  if [[ -n "$owned_root" ]]; then rm -rf -- "$owned_root"; owned_root=; fi
}
