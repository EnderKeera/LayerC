#!/usr/bin/env bash
# Runs one test command inside a disposable headless sway session: a private
# XDG_RUNTIME_DIR, no input devices, the pixman renderer and two outputs
# (HEADLESS-1 1280x720 at scale 1, HEADLESS-2 1280x720 at scale 2 to its
# right). It never touches the user's compositor, daemon or sockets. Exits 77
# (CTest skip) when sway is missing.
set -uo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 command [args...]" >&2
  exit 64
fi
if ! command -v sway >/dev/null 2>&1 || ! command -v swaymsg >/dev/null 2>&1; then
  echo "wayland-test-session: sway is missing; skipping" >&2
  exit 77
fi

# sway refuses root. Only the disposable container launcher may drop to the
# existing unprivileged account; never change users or privileges on a host.
if [[ $(id -u) == 0 ]]; then
  if [[ ${LAYERC_DISPOSABLE_ROOT:-0} != 1 ]] ||
      [[ ! -e /.dockerenv && ! -e /run/.containerenv ]]; then
    echo "wayland-test-session: run as an unprivileged user" >&2
    exit 70
  fi
  exec runuser -u nobody -- bash "$0" "$@"
fi

runtime=$(mktemp -d "${TMPDIR:-/tmp}/layerc-wayland.XXXXXX") || exit 70
chmod 700 "$runtime"
sway_pid=
cleanup() {
  if [[ -n $sway_pid ]] && kill -0 "$sway_pid" 2>/dev/null; then
    kill -TERM "$sway_pid" 2>/dev/null
    for _ in $(seq 50); do kill -0 "$sway_pid" 2>/dev/null || break; sleep 0.1; done
    kill -KILL "$sway_pid" 2>/dev/null
  fi
  [[ -n $sway_pid ]] && wait "$sway_pid" 2>/dev/null
  rm -rf -- "$runtime"
}
trap cleanup EXIT INT TERM

cat >"$runtime/sway.conf" <<'EOF'
xwayland disable
default_border none
output HEADLESS-1 resolution 1280x720 position 0 0 scale 1
output HEADLESS-2 resolution 1280x720 position 1280 0 scale 2
EOF

env -i HOME="$runtime" PATH="$PATH" XDG_RUNTIME_DIR="$runtime" \
  WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
  sway --config "$runtime/sway.conf" >"$runtime/sway.log" 2>&1 &
sway_pid=$!

socket=
ipc=
for _ in $(seq 200); do
  if ! kill -0 "$sway_pid" 2>/dev/null; then
    echo "wayland-test-session: sway exited during startup" >&2
    cat "$runtime/sway.log" >&2
    exit 70
  fi
  socket=$(find "$runtime" -maxdepth 1 -name 'wayland-[0-9]*' ! -name '*.lock' -printf '%f\n' | head -n 1)
  ipc=$(find "$runtime" -maxdepth 1 -name 'sway-ipc.*.sock' -print | head -n 1)
  [[ -n $socket && -n $ipc ]] && break
  sleep 0.05
done
if [[ -z $socket || -z $ipc ]]; then
  echo "wayland-test-session: sway did not publish its sockets" >&2
  cat "$runtime/sway.log" >&2
  exit 70
fi

if ! SWAYSOCK="$ipc" swaymsg create_output >/dev/null; then
  echo "wayland-test-session: could not create the second headless output" >&2
  exit 70
fi
for _ in $(seq 100); do
  SWAYSOCK="$ipc" swaymsg -t get_outputs -r | grep -q '"HEADLESS-2"' && break
  sleep 0.05
done

env -u DISPLAY -u WAYLAND_SOCKET XDG_RUNTIME_DIR="$runtime" WAYLAND_DISPLAY="$socket" \
  SWAYSOCK="$ipc" "$@"
status=$?
if [[ $status -ne 0 && $status -ne 77 ]]; then
  echo "--- sway log ---" >&2
  tail -n 40 "$runtime/sway.log" >&2
fi
exit "$status"
