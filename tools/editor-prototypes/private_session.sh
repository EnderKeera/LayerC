#!/usr/bin/env bash
# Sourced prototype-only X11 fixture. Callers must use set -euo pipefail.
# Own and join every process we launch; never fall back to a user's display/bus.
# Processes are owned through owned.sh: nothing here signals a numeric PID.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/owned.sh"
proto_work=
proto_cleanup() {
  owned_cleanup
  if [[ -n "$proto_work" ]]; then rm -rf -- "$proto_work"; proto_work=; fi
}
proto_init() {
  local output=${1:?output directory} screen=${2:-2880x1800x24}
  for command in Xvfb xauth xdpyinfo xwininfo openbox xprop xdotool python3 dbus-run-session mkfifo; do
    command -v "$command" >/dev/null || { echo "missing prototype dependency: $command" >&2; return 1; }
  done
  mkdir -p "$output"
  proto_work=$(mktemp -d -t layerc-editor-prototype.XXXXXXXX)
  trap proto_cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  mkdir -p "$proto_work/home" "$proto_work/config/layerc/profiles" "$proto_work/data" \
    "$proto_work/cache" "$proto_work/state" "$proto_work/runtime"
  chmod 700 "$proto_work/runtime"
  # Record the real configuration/data/state/runtime roots before replacing
  # them; the prototypes refuse any confinement grant that overlaps them.
  # Pathnames may contain any byte but NUL, so each is hex-encoded and the
  # encodings are colon-separated.
  local real_home=${HOME:-/nonexistent} root encoded=()
  for root in "${XDG_CONFIG_HOME:-$real_home/.config}" "${XDG_DATA_HOME:-$real_home/.local/share}" \
      "${XDG_STATE_HOME:-$real_home/.local/state}" "${XDG_CACHE_HOME:-$real_home/.cache}" \
      ${XDG_RUNTIME_DIR:+"$XDG_RUNTIME_DIR"}; do
    encoded+=("$(printf '%s' "$root" | od -An -v -tx1 | tr -d ' \n')")
  done
  LAYERC_PROTO_PRODUCTION_ROOTS=$(IFS=:; printf '%s' "${encoded[*]}")
  export LAYERC_PROTO_PRODUCTION_ROOTS
  export HOME="$proto_work/home" XDG_CONFIG_HOME="$proto_work/config" XDG_DATA_HOME="$proto_work/data" \
    XDG_CACHE_HOME="$proto_work/cache" XDG_STATE_HOME="$proto_work/state" \
    XDG_RUNTIME_DIR="$proto_work/runtime" XAUTHORITY="$proto_work/Xauthority"
  export LAYERC_PROTO_FIXTURE_ROOT="$proto_work"
  LAYERC_PROTO_FIXTURE_TOKEN=$(python3 -c 'import secrets; print(secrets.token_hex(32))')
  export LAYERC_PROTO_FIXTURE_TOKEN
  (umask 077; printf '%s\n' "$LAYERC_PROTO_FIXTURE_TOKEN" > "$proto_work/.layerc-prototype-fixture")
  export LAYERC_STORE_ROOT="$proto_work/config/layerc/profiles"
  unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS AT_SPI_BUS_ADDRESS NO_AT_BRIDGE XAUTHLOCALHOSTNAME SESSION_MANAGER
  export GDK_BACKEND=x11 FLTK_BACKEND=x11
  local number ready=0 lock_pid cookie server
  cookie=$(python3 -c 'import secrets; print(secrets.token_hex(16))')
  for _ in $(seq 10); do
    number=$((1000 + RANDOM % 20000))
    export DISPLAY=":$number"
    xauth -f "$XAUTHORITY" add "$DISPLAY" MIT-MAGIC-COOKIE-1 "$cookie" 2>/dev/null
    # Explicit numbering supports WSLg's read-only .X11-unix via abstract sockets.
    owned_start xvfb "$output/xvfb.log" -- Xvfb "$DISPLAY" -auth "$XAUTHORITY" \
      -screen 0 "$screen" -nolisten tcp || { owned_release xvfb || true; continue; }
    server=$(owned_pid xvfb)
    for _ in $(seq 50); do
      owned_alive xvfb || break
      if [[ -r "/tmp/.X$number-lock" ]] && read -r lock_pid < "/tmp/.X$number-lock" &&
         [[ "$lock_pid" == "$server" ]] && xdpyinfo >/dev/null 2>&1; then
        ready=1; break
      fi
      sleep .1
    done
    [[ "$ready" == 1 ]] && break
    owned_release xvfb || true
  done
  [[ "$ready" == 1 ]] || { echo 'private authenticated Xvfb did not start' >&2; return 1; }
}
proto_run() {
  # A fresh session owns the private bus and all of its helpers. The group
  # supervisor signals that group only while its leader is still unreaped.
  local result=0
  owned_start session - --group -- dbus-run-session -- "$@" || return 1
  owned_wait session || result=$?
  return "$result"
}
