#!/usr/bin/env bash
# Shared shell mechanics for Linux smoke and evidence scripts. Callers own their
# daemon/control commands, readiness budget, diagnostics, and evidence schema.

# start <control-function> <log> <status> <tries> <seconds> <timeout-error>
#       <early-exit-error-or-empty> <daemon-command...>
layerc_daemon_start() {
  local ctl_command=$1 log=$2 status_file=$3 attempts=$4
  local interval=$5 timeout_error=$6 exit_error=$7
  local attempt
  shift 7

  "$@" >"$log" 2>&1 &
  layerc_daemon_pid=$!
  for ((attempt = 0; attempt < attempts; ++attempt)); do
    if ! kill -0 "$layerc_daemon_pid" 2>/dev/null; then
      if [[ -n $exit_error ]]; then
        echo "$exit_error" >&2
        return 1
      fi
      break
    fi
    if "$ctl_command" status --json >"$status_file" 2>/dev/null; then
      return 0
    fi
    sleep "$interval"
  done
  echo "$timeout_error" >&2
  return 1
}

layerc_daemon_cleanup() {
  local ctl_command=$1
  if [[ -n ${layerc_daemon_pid:-} ]] && kill -0 "$layerc_daemon_pid" 2>/dev/null; then
    "$ctl_command" stop >/dev/null 2>&1 || kill "$layerc_daemon_pid" 2>/dev/null || true
    wait "$layerc_daemon_pid" 2>/dev/null || true
  fi
  layerc_daemon_pid=
}

layerc_daemon_stop() {
  local ctl_command=$1
  "$ctl_command" stop
  wait "$layerc_daemon_pid"
  layerc_daemon_pid=
}

# x11-exec <tries> <seconds> <command...>
# Runs the command once the X server in $DISPLAY accepts connections. xvfb-run
# can return before a fresh Xvfb takes its first client, and a suite that finds
# no display skips instead of failing, which the evidence gate then rejects.
layerc_x11_exec() {
  local attempts=$1 interval=$2 attempt
  shift 2
  if ! command -v xdpyinfo >/dev/null; then
    echo 'error: xdpyinfo (x11-utils) is required to probe the X display' >&2
    return 1
  fi
  for ((attempt = 0; attempt < attempts; ++attempt)); do
    if xdpyinfo >/dev/null 2>&1; then
      "$@"
      return
    fi
    sleep "$interval"
  done
  echo "error: X display ${DISPLAY:-<unset>} did not accept connections" >&2
  return 1
}

layerc_write_sha256sums() {
  local evidence_dir=$1 checksums rc
  checksums=$(mktemp) || return 1
  if (
    cd "$evidence_dir" &&
      find . -type f ! -name SHA256SUMS -print0 | LC_ALL=C sort -z | xargs -0 sha256sum
  ) >"$checksums" && mv -- "$checksums" "$evidence_dir/SHA256SUMS"; then
    return 0
  else
    rc=$?
    rm -f -- "$checksums"
    return "$rc"
  fi
}
