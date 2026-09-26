#!/usr/bin/env bash
set -Eeuo pipefail
tools_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
for script in "$tools_dir/linux-lib.sh" "$tools_dir/linux-runtime-smoke.sh" \
    "$tools_dir/run-linux-v26-evidence.sh" "$tools_dir/run-linux-tests.sh" \
    "$tools_dir/tests/test_linux_lib.sh"; do
  bash -n "$script"
done
source "$tools_dir/linux-lib.sh"

root=$(mktemp -d)
layerc_daemon_pid=
fake_ctl() {
  case "$1" in
    status) [[ $2 == --json && -f $root/ready ]] || return 1
      printf '{"ready":true}\n' ;;
    stop) : >"$root/stop" ;;
    *) return 2 ;;
  esac
}
fake_daemon() {
  while [[ ! -f $root/stop ]]; do sleep 0.01; done
}
cleanup() {
  layerc_daemon_cleanup fake_ctl
  rm -rf -- "$root"
}
trap cleanup EXIT
fail() { echo "error: $*" >&2; exit 1; }

# Readiness can follow multiple failed probes; a normal stop must reap the child.
(sleep 0.05; : >"$root/ready") &
ready_pid=$!
layerc_daemon_start fake_ctl "$root/daemon.log" "$root/status.json" \
  50 0.02 'timeout' '' fake_daemon
wait "$ready_pid"
[[ $(<"$root/status.json") == '{"ready":true}' ]] || fail 'missing ready status'
pid=$layerc_daemon_pid
layerc_daemon_stop fake_ctl
[[ -z $layerc_daemon_pid && -f $root/stop ]] || fail 'clean stop failed'
kill -0 "$pid" 2>/dev/null && fail 'clean stop left daemon running'

# Timeout and early exit keep their caller-specific diagnostics and can clean up.
rm -f -- "$root/ready" "$root/stop"
if layerc_daemon_start fake_ctl "$root/daemon.log" "$root/status.json" \
    2 0.01 'smoke timeout' '' fake_daemon 2>"$root/error.txt"; then
  fail 'unready daemon accepted'
fi
[[ $(<"$root/error.txt") == 'smoke timeout' ]] || fail 'wrong timeout error'
layerc_daemon_cleanup fake_ctl
if layerc_daemon_start fake_ctl "$root/daemon.log" "$root/status.json" \
    10 0.01 'physical timeout' 'physical exit' false 2>"$root/error.txt"; then
  fail 'exited daemon accepted'
fi
[[ $(<"$root/error.txt") == 'physical exit' ]] || fail 'wrong early-exit error'
layerc_daemon_cleanup fake_ctl

# An argv-style launcher covers Flatpak's options without invoking Flatpak or input.
mock_launcher() {
  [[ $# -eq 4 && $1 == run && $2 == --user &&
     $3 == --command=layerd && $4 == app:id ]] || return 2
  fake_daemon
}
: >"$root/ready"
rm -f -- "$root/stop"
layerc_daemon_start fake_ctl "$root/daemon.log" "$root/status.json" \
  10 0.01 'launcher timeout' 'launcher exit' \
  mock_launcher run --user --command=layerd app:id
layerc_daemon_stop fake_ctl

# On a failed control stop, cleanup must kill and reap its own daemon.
failing_ctl() {
  [[ $1 != stop ]] || return 1
  fake_ctl "$@"
}
rm -f -- "$root/stop"
layerc_daemon_start failing_ctl "$root/daemon.log" "$root/status.json" \
  10 0.01 'cleanup timeout' '' fake_daemon
pid=$layerc_daemon_pid
layerc_daemon_cleanup failing_ctl
[[ -z $layerc_daemon_pid ]] || fail 'cleanup did not reset pid'
kill -0 "$pid" 2>/dev/null && fail 'cleanup left daemon running'

# The manifest includes summary.json, excludes itself, and has byte-stable order.
evidence="$root/evidence with spaces"
mkdir -p "$evidence/nested"
printf '{"schemaVersion":1}\n' >"$evidence/summary.json"
printf 'a\n' >"$evidence/a file"
printf 'z\n' >"$evidence/nested/z"
printf 'stale\n' >"$evidence/SHA256SUMS"
layerc_write_sha256sums "$evidence"
(cd "$evidence" && sha256sum -c SHA256SUMS >/dev/null)
expected=$(printf '%s\n' './a file' './nested/z' './summary.json')
actual=$(sed 's/^[0-9a-f]\{64\} [ *]//' "$evidence/SHA256SUMS")
[[ $actual == "$expected" ]] || fail 'checksum file order or contents changed'
cp "$evidence/SHA256SUMS" "$root/first-sums"
layerc_write_sha256sums "$evidence"
cmp -s "$root/first-sums" "$evidence/SHA256SUMS" || fail 'checksums were not stable'
if layerc_write_sha256sums "$root/missing" 2>"$root/error.txt"; then
  fail 'missing evidence directory accepted'
fi
cmp -s "$root/first-sums" "$evidence/SHA256SUMS" || fail 'failed seal changed evidence'
# The X11 gate waits for the display, passes the command's status through, and
# fails without running the command when the display never answers.
xdpyinfo() { [[ -f $root/display-up ]]; }
(sleep 0.05; : >"$root/display-up") &
display_pid=$!
layerc_x11_exec 50 0.02 bash -c 'exit 3' && fail 'command status was dropped'
[[ $? -eq 3 ]] || fail 'command status changed'
wait "$display_pid"
rm -f -- "$root/display-up"
if layerc_x11_exec 3 0.01 touch "$root/ran" 2>/dev/null; then
  fail 'unreachable display accepted'
fi
[[ ! -e $root/ran ]] || fail 'command ran without a display'
unset -f xdpyinfo
printf 'linux-lib smoke: pass\n'
