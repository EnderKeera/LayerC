#!/usr/bin/env bash
# Disposable X11 GUI runner for the Linux LayerE editor (Debian WSL or Linux).
# Single Linux owner of private-session setup, owned helpers, cases, gates, reports, and
# cleanup; the PowerShell entry point only mirrors/builds/proxies.
#
#   linux-editor-gui-tests.sh --build DIR --out DIR [--mode walkthrough|session|resources|peaks|limits|wayland]
#                             [--idle-seconds N] [--session-seconds N] [--env FILE]
#                             [--expect-stamp FILE]
#                             [--limit-inputs DIR --limit-case GROUP:ID]
# limits: one frozen validator-derived boundary document; real edits/import,
#   full retained history, explicit refusal/recovery and continuous sampled peak.
#
# walkthrough (default): run tools/linux-gui/walkthrough.py through the bridge
#   and gate on its required cases, the editor resource targets and cleanup.
# wayland: the editor as an XWayland client of a private headless sway, with a
#   native foot window beside the X11 fixture; runs linux-gui/wayland_walkthrough.py
#   and gates on accurate native/degraded reporting. XWayland needs a writable
#   sticky /tmp/.X11-unix (under WSLg, run inside a private tmpfs there).
# session: keep the computer-use bridge open for one external agent until it
#   sends {"op":"stop"} or the bridge idles out (--session-seconds).
#
# Everything runs in a private authenticated Xvfb + Openbox + D-Bus session
# with private HOME/XDG roots: it never addresses the user's display, bus,
# daemon, control socket or profile store. Every helper is owned and joined
# (tools/linux-gui/owned.sh). Dependency installation is separate;
# a missing dependency fails the run instead of skipping it. --env sources a
# dependency-root setup script (for example an extracted sysroot) first.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/linux-lib.sh"
source "$here/linux-gui/owned.sh"

# wait_for SECONDS COMMAND...: retry until success, bounded by a monotonic timeout.
wait_for() {
  local seconds=$1
  shift
  timeout "$seconds" bash -c 'until "$@" >/dev/null 2>&1; do sleep .05; done' -- "$@"
}

inside() {
  local out=$GUI_OUT build=$GUI_BUILD work=$GUI_WORK result=0
  local profiles="$XDG_CONFIG_HOME/layerc/profiles" session="$GUI_WORK/bridge"
  finish_inside() {
    local rc=$? i
    trap - EXIT
    for ((i=${#owned_names[@]}-1; i>=0; i--)); do
      owned_release "${owned_names[$i]}" || { ((rc)) || rc=70; }
    done
    # Runs after inside() returned: use globals, not its locals.
    mkdir -p "$GUI_OUT/ownership"
    [[ -z "$owned_root" ]] || cp "$owned_root"/*.status "$GUI_OUT/ownership/" 2>/dev/null || true
    owned_cleanup
    exit "$rc"
  }
  trap finish_inside EXIT

  if [[ $GUI_MODE == wayland ]]; then
    # sway manages windows; a native Wayland client sits beside the XWayland
    # fixture so the native/degraded split is observable.
    # foot reports a harness stop as status 230; the wrapper turns only that
    # requested stop into success, so a crash still fails the run.
    owned_start native "$out/native.log" -- bash -c \
      'foot --title "LayerC Native Window" sleep infinity & child=$!
       trap "kill -TERM $child; wait $child; exit 0" TERM; wait $child'
  else
    owned_start wm "$out/openbox.log" -- openbox --sm-disable
  fi
  [[ $GUI_MODE == wayland ]] ||
    wait_for 10 bash -c 'xprop -root _NET_SUPPORTING_WM_CHECK | grep -q "window id"'

  mkdir -p "$profiles"
  python3 - "$profiles" "$GUI_MODE" "$here/linux-gui" "$out" <<'PY'
import copy, json, os, sys
from pathlib import Path
profile = {"schemaVersion": 4, "id": "fixture", "name": "GUI fixture", "enabled": True,
           "modules": [], "hidBindings": [], "cooldownGroups": [],
           "game": {"executableName": "lctest.exe"},
           "cooldowns": [{"id": "ability", "name": "Ability", "durationMs": 8000,
                          "maxCharges": 1, "retrigger": "ignore", "recovery": "single"}],
           "panels": [{"id": "main", "name": "Main", "layout": "row", "style": "bar",
                       "bar": {"width": 360, "height": 30, "fillColor": "#50BAC8",
                               "backgroundColor": "#202A31", "borderColor": "#50BAC8",
                               "textColor": "#F1F5F7", "hideWhenReady": False},
                       "anchorX": 0.25, "anchorY": 0.75, "cooldowns": ["ability"]}],
           "bindings": [{"cooldownId": "ability",
                         "chord": [{"device": "keyboard", "code": 69}]}]}
# Forward-compatible data must survive the actual editor/autosave/export paths.
profile["futureProfile"] = {"large": 18446744073709551615, "note": "opaque metadata"}
profile["cooldowns"][0]["futureCooldown"] = {"keep": True}
store = Path(sys.argv[1])
sys.path.insert(0, sys.argv[3])
if sys.argv[2] == "peaks":
    from peak_fixture import write_representative
    write_representative(profile, store, Path(sys.argv[4]))
elif sys.argv[2] == "limits":
    from limit_inputs import prepare
    prepare(Path(os.environ["GUI_LIMIT_INPUTS"]), os.environ["GUI_LIMIT_CASE"],
            store, Path(sys.argv[4]))
elif sys.argv[2] != "resources":
    (store / "fixture.json").write_text(json.dumps(profile))
    if sys.argv[2] == "walkthrough":
        legacy = copy.deepcopy(profile)
        legacy.update(schemaVersion=3, id="legacy", name="Legacy autosave fixture", enabled=False)
        legacy["bindings"].append({"cooldownId": "ability",
                                   "chord": [{"device": "mouse", "code": 4}]})
        del legacy["cooldownGroups"]  # This member did not exist before schema 4.
        (store / "y-legacy.json").write_text(json.dumps(legacy))
        shared = copy.deepcopy(profile)
        shared.update(id="shared", name="Shared calibration fixture", enabled=False,
                      modules=[{"kind": "visual", "schemaVersion": 2}])
        sibling = copy.deepcopy(shared["cooldowns"][0])
        sibling.update(id="sibling", name="Sibling")
        shared["cooldowns"].append(sibling)
        shared["bindings"].append({"cooldownId": "sibling",
                                   "chord": [{"device": "keyboard", "code": 70}]})
        (store / "z-shared.json").write_text(json.dumps(shared))
        visual = {"schemaVersion": 2, "profileId": "shared", "sampleRateHz": 5, "quality": "balanced",
                  "observations": [{"type": "pixel", "id": "shared-observation", "name": "Shared pixel",
                                    "region": {"x": 0.02, "y": 0.02, "width": 0.2, "height": 0.2},
                                    "predicate": {"type": "averageColor", "target": "#808080", "tolerance": 10},
                                    "stableSamples": 2, "repeat": "edge"}],
                  "triggers": [{"cooldownId": cid, "observationId": "shared-observation", "enabled": True,
                                "mode": "pixelOnly", "keepInput": True, "onUnavailable": "ignore"}
                               for cid in ("ability", "sibling")]}
        # Nondefault sibling controls must survive shared update, clone and removal.
        visual["triggers"][1] = {"cooldownId": "sibling", "observationId": "shared-observation",
                                 "enabled": False, "mode": "confirmInput",
                                 "confirmationTimeoutMs": 1337, "onUnavailable": "triggerFromInput"}
        data = Path(os.environ["XDG_DATA_HOME"]) / "layerc" / "profiles" / "z-shared"
        data.mkdir(parents=True)
        (data / "visual.json").write_text(json.dumps(visual))
else:
    # The approved representative store: 20 profiles, each with ten
    # cooldowns and two panels; the first (fixture.json) opens selected.
    for number in range(1, 21):
        item = copy.deepcopy(profile)
        item.update(id=f"fixture-{number:02d}", name=f"Fixture {number:02d}")
        ids = [f"cooldown-{c:02d}" for c in range(1, 11)]
        item["cooldowns"] = [{"id": cid, "name": f"Cooldown {c}", "durationMs": 1000 * c,
                              "maxCharges": 1 + c % 3, "retrigger": "ignore", "recovery": "single"}
                             for c, cid in enumerate(ids, 1)]
        second = copy.deepcopy(item["panels"][0])
        second.update(id="side", name="Side", anchorX=0.75, anchorY=0.25)
        item["panels"][0]["cooldowns"], second["cooldowns"] = ids[:5], ids[5:]
        item["panels"].append(second)
        item["bindings"] = [{"cooldownId": ids[0], "chord": [{"device": "keyboard", "code": 69}]}]
        name = "fixture.json" if number == 1 else f"fixture-{number:02d}.json"
        (store / name).write_text(json.dumps(item))
PY
  cp "$profiles/fixture.json" "$out/profile-before.json"

  cp "$build/layerc_linux_x11_smoke_fixture" "$work/lctest.exe"
  owned_start fixture "$out/fixture.log" -- "$work/lctest.exe"
  wait_for 10 xdotool search --name '^LayerC Automation Fixture$'
  local game
  game=$(xdotool search --name '^LayerC Automation Fixture$' | head -n1)
  xprop -id "$game" -f _NET_WM_PID 32c -set _NET_WM_PID "$(owned_pid fixture)"
  xdotool windowmove "$game" 60 80 windowsize "$game" 1100 650

  # LayerD runs under an owned restart loop: after a stop it waits on this
  # FIFO, so the bridge can start it again for reconnect cases without ever
  # spawning a process itself. Release stops the loop's whole group.
  mkfifo -m 600 "$work/daemon.control"
  owned_start daemon "$out/daemon.log" -- bash -c \
    'while "$1"; read -r request < "$2" && [[ $request == start ]]; do :; done' \
    -- "$build/layerd" "$work/daemon.control"
  wait_for 10 "$build/layerctl" status --json

  # The editor's AT-SPI bridge is demand-driven: while org.a11y.Status
  # IsEnabled is false it stays idle by design, so this private session bus
  # (never the user's) is marked enabled before layere starts. gdbus activates
  # the bus launcher here; layere never asks the bus to auto-start it.
  if ! gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus \
      --method org.freedesktop.DBus.Properties.Set org.a11y.Status IsEnabled \
      '<true>' >"$out/a11y-enable.log" 2>&1; then
    echo "error: could not enable org.a11y.Status on the private session bus" >&2
    cat "$out/a11y-enable.log" >&2
    return 1
  fi

  local nonce editor_pid editor_start editor
  # The editor's working directory is disposable, never the source mirror.
  cd "$work"
  nonce=$(python3 -c 'import secrets; print(secrets.token_hex(16))')
  local lifetime_args=()
  [[ $GUI_MODE != peaks && $GUI_MODE != limits ]] || lifetime_args=(--lifetime-report "$out/active-resources.json")
  owned_start sampler "$out/sampler.log" -- python3 "$here/linux-gui/start_process.py" \
    "$build/layere" "$out/editor.log" "$work/editor.pid" "$out/startup-resources.json" 1 "$nonce" \
    "${lifetime_args[@]}"
  wait_for 10 test -s "$work/editor.pid"
  read -r editor_pid editor_start < "$work/editor.pid"
  wait_for 10 xdotool search --all --pid "$editor_pid" --name '^LayerE'
  editor=$(xdotool search --all --pid "$editor_pid" --name '^LayerE' | head -n1)
  # Harness readiness marker, written after the owned window is observed;
  # production LayerE emits no test-only readiness text.
  printf '\nready\n' >> "$out/editor.log"
  wait_for 10 test -s "$out/startup-resources.json"
  # An error report is also nonempty: require sampler success and exact
  # identity before addressing a window that its failed owner may have closed.
  python3 "$here/linux-gui/startup_readiness.py" "$out/startup-resources.json" \
    "$editor_pid" "$editor_start" "$nonce"
  xdotool windowmove "$editor" 20 20
  printf '{"nonce":"%s","pid":%s,"startTime":"%s"}\n' "$nonce" "$editor_pid" "$editor_start" \
    > "$out/editor-identity.json"

  python3 "$here/linux-gui/sample.py" idle "$editor_pid" "$GUI_IDLE" "$out/resources.json"

  mkdir -m 700 "$session"
  owned_start bridge "$out/bridge.log" -- python3 "$here/linux-gui/bridge.py" serve \
    --session "$session" --evidence "$out" --display "$DISPLAY" --profile-dir "$profiles" \
    --layerctl "$build/layerctl" --daemon-control "$work/daemon.control" \
    --xvfb-pid "$GUI_XVFB_PID" --idle-seconds "$GUI_SESSION_SECONDS"
  # The bridge publishes its token before the socket: a socket means ready.
  wait_for 10 test -S "$session/bridge.sock"
  test -s "$session/bridge.token"
  python3 "$here/linux-gui/sample.py" roles "$out/harness-resources.json" \
    "$([[ $GUI_MODE == wayland ]] && echo sway || echo xvfb)=$GUI_XVFB_PID" \
    "$([[ $GUI_MODE == wayland ]] && echo "foot=$(owned_pid native)" || echo "openbox=$(owned_pid wm)")" \
    "layerd=$(pgrep -P "$(owned_pid daemon)" -x layerd)" \
    "fixture=$(owned_pid fixture)" "bridge=$(owned_pid bridge)"

  if [[ $GUI_MODE == walkthrough ]]; then
    python3 "$here/linux-gui/walkthrough.py" --session "$session" --out "$out" || result=$?
    python3 "$here/linux-gui/bridge.py" call --session "$session" '{"op":"stop"}' >/dev/null
  elif [[ $GUI_MODE == wayland ]]; then
    python3 "$here/linux-gui/wayland_walkthrough.py" --session "$session" --out "$out" || result=$?
    python3 "$here/linux-gui/bridge.py" call --session "$session" '{"op":"stop"}' >/dev/null
  elif [[ $GUI_MODE == peaks ]]; then
    python3 "$here/linux-gui/active_walkthrough.py" --session "$session" --out "$out" || result=$?
    python3 "$here/linux-gui/bridge.py" call --session "$session" '{"op":"stop"}' >/dev/null
  elif [[ $GUI_MODE == limits ]]; then
    python3 "$here/linux-gui/limits_walkthrough.py" --session "$session" --out "$out" || result=$?
    python3 "$here/linux-gui/bridge.py" call --session "$session" '{"op":"stop"}' >/dev/null
  elif [[ $GUI_MODE == resources ]]; then
    python3 "$here/linux-gui/cycles.py" --session "$session" --out "$out" || result=$?
    python3 "$here/linux-gui/bridge.py" call --session "$session" '{"op":"stop"}' >/dev/null
  else
    python3 - "$out/session.json" "$session" "$DISPLAY" "$GUI_SESSION_SECONDS" <<'PY'
import json, os, sys
temporary = sys.argv[1] + ".tmp"
with open(temporary, "w") as handle:  # Published atomically: readers never see a partial file.
    json.dump({"sessionDir": sys.argv[2], "display": sys.argv[3],
               "idleSeconds": float(sys.argv[4]),
               "call": "python3 tools/linux-gui/bridge.py call --session SESSION REQUEST"},
              handle, indent=2)
os.replace(temporary, sys.argv[1])
PY
    echo "bridge session ready: $session"
  fi
  owned_wait bridge || result=$?
  owned_stop sampler || result=$?
  [[ ! -f "$profiles/fixture.json" ]] || cp "$profiles/fixture.json" "$out/profile-after.json"
  return "$result"
}

if [[ ${1:-} == --inside ]]; then
  inside
  exit
fi

# WSLENV can forward a Windows HOME; resolve the account's own home.
HOME=$(getent passwd "$(id -u)" | cut -d: -f6)
export HOME
mode=walkthrough build= out= idle_seconds=60 session_seconds=1800 env_file= expect_stamp= limit_inputs= limit_case=
while (($#)); do
  case $1 in
    --build) build=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --mode) mode=$2; shift 2 ;;
    --idle-seconds) idle_seconds=$2; shift 2 ;;
    --session-seconds) session_seconds=$2; shift 2 ;;
    --env) env_file=$2; shift 2 ;;
    --expect-stamp) expect_stamp=$2; shift 2 ;;
    --limit-inputs) limit_inputs=$2; shift 2 ;;
    --limit-case) limit_case=$2; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 64 ;;
  esac
done
[[ -n $build && -n $out ]] || { echo "usage: $0 --build DIR --out DIR [--mode walkthrough|session|resources|peaks|limits|wayland]" >&2; exit 64; }
[[ $mode == walkthrough || $mode == session || $mode == resources || $mode == peaks || $mode == limits || $mode == wayland ]] ||
  { echo "unknown mode: $mode" >&2; exit 64; }
if [[ $mode == limits ]]; then
  [[ -n $limit_inputs && -n $limit_case ]] || { echo "limits requires --limit-inputs DIR --limit-case ID" >&2; exit 64; }
  limit_inputs=$(realpath "$limit_inputs")
else
  [[ -z $limit_inputs && -z $limit_case ]] || { echo "boundary arguments require limits mode" >&2; exit 64; }
fi
expand_home() { [[ $1 == "~"* ]] && printf '%s' "$HOME${1:1}" || printf '%s' "$1"; }
build=$(realpath "$(expand_home "$build")")
# Hold the mirror/build pair for the whole run, so no build replaces the
# executables or helper scripts underneath it (see tools/linux-gui/build.sh).
# The scripts themselves run from the mirror ($here is its tools/ directory).
exec 8>>"$(dirname "$here").lock" 9>>"$build.lock"
if ! flock -s -n 8 || ! flock -s -n 9; then
  echo "a build is replacing $build; retry when it finishes" >&2
  exit 75
fi
# Under the shared locks, the executables must still be the ones this run's
# build produced: another build may have finished in between.
if [[ -n $expect_stamp ]] && ! (cd "$build" && sha256sum --check --quiet --status "$expect_stamp"); then
  echo "executables in $build changed since this run's build; rerun" >&2
  exit 75
fi
if [[ -n $env_file ]]; then
  # shellcheck source=/dev/null
  source "$(expand_home "$env_file")"
fi
out=$(realpath -m "$out")
mkdir -p "$out"
[[ ! -e "$out/result.json" ]] || { echo "refusing to reuse evidence directory: $out" >&2; exit 64; }

work= session_exit=1 setup_error= xvfb_name= display_role=xvfb
attempt_numbers=() attempt_pids=()
finish() {
  local rc=$? joined=1 sandbox_removed=1 leftovers lock_left=0 stale_locks= i name status lock_pid
  trap - EXIT
  # Stop and join first, so the final statuses (including stop failures)
  # are preserved before the ownership metadata is removed. A release's exit
  # code is the child's own result (judged by report.py from its status);
  # only a supervisor that failed, or left no status, is a join failure.
  for ((i=${#owned_names[@]}-1; i>=0; i--)); do
    owned_release "${owned_names[$i]}" || true
  done
  for name in "${owned_names[@]}"; do
    status="$owned_root/$name.status"
    if [[ ! -e "$status" ]] || grep -q '"state": "failed"' "$status"; then joined=0; fi
  done
  mkdir -p "$out/ownership"
  if [[ -n "$owned_root" ]]; then
    for status in "$owned_root"/*.status; do
      [[ -e "$status" ]] || continue
      name=$(basename "$status" .status)
      [[ $name == "$xvfb_name" ]] && name=$display_role
      [[ $name == xvfb-attempt-* ]] || name="outer-$name"
      cp "$status" "$out/ownership/$name.status" || joined=0
    done
  fi
  owned_cleanup
  leftovers=
  if [[ -n "$work" ]]; then
    leftovers=$(pgrep -u "$(id -u)" -f -- "$work" | tr '\n' ' ' || true)
    rm -rf -- "$work" || true
    [[ ! -e "$work" ]] || sandbox_removed=0
  fi
  # Every display this run tried must be released. A lock naming another
  # server's PID is not ours to judge or remove. One naming our own, now
  # joined Xvfb (left behind by a crash or SIGKILL) is stale: remove and
  # record it. One whose PID is still an Xvfb was not released.
  for i in "${!attempt_numbers[@]}"; do
    lock="/tmp/.X${attempt_numbers[$i]}-lock"
    if [[ -r "$lock" ]] && read -r lock_pid < "$lock" && [[ "$lock_pid" == "${attempt_pids[$i]}" ]]; then
      if tr '\0' ' ' < "/proc/$lock_pid/cmdline" 2>/dev/null | grep -q '^Xvfb '; then
        lock_left=1
      elif rm -f -- "$lock"; then
        stale_locks+="${attempt_numbers[$i]} "
      else
        lock_left=1
      fi
    fi
  done
  ((rc == 0)) || [[ -n "$setup_error" ]] || setup_error="runner exited with status $rc"
  python3 "$here/linux-gui/report.py" "$out" "$mode" "$session_exit" "$joined" \
    "$sandbox_removed" "$leftovers" "$lock_left" "$stale_locks" "$setup_error" || rc=1
  layerc_write_sha256sums "$out" || rc=1
  # Publication is the final handoff. A Windows WSL launcher exiting is not
  # proof that the Linux cleanup/report has finished. Bind both final files.
  if [[ -f "$out/result.json" && -f "$out/SHA256SUMS" ]]; then
    local result_sha manifest_sha
    result_sha=$(sha256sum -- "$out/result.json") || rc=1
    manifest_sha=$(sha256sum -- "$out/SHA256SUMS") || rc=1
    printf '{"version":1,"exitCode":%d,"resultSha256":"%s","manifestSha256":"%s"}\n' \
      "$rc" "${result_sha%% *}" "${manifest_sha%% *}" > "$out/.runner-complete.tmp" || rc=1
    mv -fT -- "$out/.runner-complete.tmp" "$out/runner-complete.json" || rc=1
  fi
  exit "$rc"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

display_commands=(Xvfb xauth openbox)
[[ $mode != wayland ]] || display_commands=(sway Xwayland foot)
for command in "${display_commands[@]}" xdpyinfo xprop xwininfo xev stdbuf xdotool xmodmap import convert python3 \
    dbus-run-session gdbus pgrep sha256sum timeout; do
  if ! command -v "$command" >/dev/null; then
    setup_error="missing required dependency: $command"
    echo "$setup_error" >&2
    exit 1
  fi
done
for program in layere layerd layerctl layerc_linux_x11_smoke_fixture; do
  if [[ ! -x "$build/$program" ]]; then
    setup_error="missing required build output: $program"
    echo "$setup_error" >&2
    exit 1
  fi
done

work=$(mktemp -d -t layerc-gui.XXXXXXXX)
chmod 700 "$work"
mkdir -p "$work/home" "$work/config" "$work/data" "$work/cache" "$work/state" "$work/runtime"
chmod 700 "$work/runtime"
export HOME="$work/home" XDG_CONFIG_HOME="$work/config" XDG_DATA_HOME="$work/data" \
  XDG_CACHE_HOME="$work/cache" XDG_STATE_HOME="$work/state" XDG_RUNTIME_DIR="$work/runtime" \
  XAUTHORITY="$work/Xauthority" FLTK_BACKEND=x11
unset WAYLAND_DISPLAY WAYLAND_SOCKET DBUS_SESSION_BUS_ADDRESS AT_SPI_BUS_ADDRESS NO_AT_BRIDGE \
  LAYERC_PROFILE_DIR LAYERC_STORE_ROOT SESSION_MANAGER
if [[ $mode == wayland ]]; then
  # Detection must find the private sway: LayerD runs as in a Wayland session.
  export LAYERC_DISPLAY_BACKEND=auto XDG_SESSION_TYPE=wayland
else
  export LAYERC_DISPLAY_BACKEND=x11
fi

python3 - "$out" "$build" <<'PY'
import hashlib, json, os, pathlib, platform, subprocess, sys
out, build = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
def first(command):
    try:
        # Inside the private HOME/XDG roots, with no display to touch.
        env = {k: v for k, v in os.environ.items() if k != "DISPLAY"}
        run = subprocess.run(command, capture_output=True, text=True, timeout=10, env=env)
        return (run.stdout + run.stderr).strip().splitlines()[0]
    except (OSError, IndexError, subprocess.TimeoutExpired):
        return None
names = ("layere", "layerd", "layerctl", "layerc_linux_x11_smoke_fixture")
(out / "executables.json").write_text(json.dumps(
    {n: hashlib.sha256((build / n).read_bytes()).hexdigest() for n in names}, indent=2) + "\n")
(out / "environment.json").write_text(json.dumps({
    "kernel": platform.uname()._asdict(), "python": platform.python_version(),
    "layere": first([str(build / "layere"), "--version"]),
    "xvfb": first(["Xvfb", "-version"]), "openbox": first(["openbox", "--version"]),
    "sway": first(["sway", "--version"]), "foot": first(["foot", "--version"]),
    "xdotool": first(["xdotool", "version"]), "imagemagick": first(["convert", "-version"]),
    "screen": "1600x1000x24"}, indent=2) + "\n")
PY

# Private authenticated display. Explicit numbers use abstract sockets, which
# also works where /tmp/.X11-unix is read-only (WSLg).
if [[ $mode == wayland ]]; then
  # Private headless sway with XWayland inside this run's runtime directory.
  if [[ ! -k /tmp/.X11-unix || ! -w /tmp/.X11-unix ]]; then
    setup_error="XWayland needs a writable sticky /tmp/.X11-unix"
    echo "$setup_error" >&2
    exit 1
  fi
  cat > "$work/sway.conf" <<EOF
xwayland enable
default_border none
for_window [class=".*"] floating enable
for_window [app_id=".*"] floating enable
output HEADLESS-1 resolution 1600x1000 position 0 0 scale 1
exec printf '%s' "\$DISPLAY" > "$work/xwayland.display"
EOF
  xvfb_name=sway display_role=compositor
  # Its own process group holds XWayland too: release stops both.
  owned_start sway "$out/sway.log" --group -- env WLR_BACKENDS=headless WLR_RENDERER=pixman \
    WLR_LIBINPUT_NO_DEVICES=1 sway --config "$work/sway.conf"
  xvfb_pid=$(owned_pid sway)
  ready=0
  for _ in $(seq 200); do
    owned_alive sway || break
    socket=$(find "$XDG_RUNTIME_DIR" -maxdepth 1 -name 'wayland-[0-9]*' ! -name '*.lock' \
      -printf '%f\n' | head -n 1)
    if [[ -n $socket && -s $work/xwayland.display ]]; then
      export WAYLAND_DISPLAY=$socket DISPLAY=$(cat "$work/xwayland.display")
      if xdpyinfo >/dev/null 2>&1; then ready=1; break; fi
    fi
    sleep .1
  done
  if ((!ready)); then
    setup_error="private headless sway with XWayland did not start"
    exit 1
  fi
else
cookie=$(python3 -c 'import secrets; print(secrets.token_hex(16))')
ready=0
for attempt in $(seq 10); do
  display_number=$((1000 + RANDOM % 20000))
  [[ ! -e "/tmp/.X$display_number-lock" ]] || continue
  export DISPLAY=":$display_number"
  xauth -f "$XAUTHORITY" add "$DISPLAY" MIT-MAGIC-COOKIE-1 "$cookie" 2>/dev/null
  xvfb_name="xvfb-attempt-$attempt"
  owned_start "$xvfb_name" "$out/$xvfb_name.log" -- Xvfb "$DISPLAY" -auth "$XAUTHORITY" \
    -screen 0 1600x1000x24 -nolisten tcp || { owned_release "$xvfb_name" || true; continue; }
  xvfb_pid=$(owned_pid "$xvfb_name")
  attempt_numbers+=("$display_number")
  attempt_pids+=("$xvfb_pid")
  for _ in $(seq 50); do
    owned_alive "$xvfb_name" || break
    if [[ -r "/tmp/.X$display_number-lock" ]] && read -r lock_pid < "/tmp/.X$display_number-lock" &&
       [[ "$lock_pid" == "$xvfb_pid" ]] && xdpyinfo >/dev/null 2>&1; then
      ready=1
      break
    fi
    sleep .1
  done
  ((ready)) && break
  owned_release "$xvfb_name" || true
done
if ((!ready)); then
  xvfb_name=
  setup_error="private authenticated Xvfb did not start"
  exit 1
fi

fi

export GUI_OUT="$out" GUI_BUILD="$build" GUI_WORK="$work" GUI_MODE="$mode" \
  GUI_IDLE="$idle_seconds" GUI_SESSION_SECONDS="$session_seconds" GUI_XVFB_PID="$xvfb_pid" \
  GUI_LIMIT_INPUTS="$limit_inputs" GUI_LIMIT_CASE="$limit_case"
session_exit=0
owned_start session "$out/session.log" --group -- dbus-run-session -- bash "$here/linux-editor-gui-tests.sh" --inside
owned_wait session || session_exit=$?
exit 0
