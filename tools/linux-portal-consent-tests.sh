#!/usr/bin/env bash
# Real GNOME source-picker deny/cancel/revoke in an OWNED headless Wayland
# namespace. Never addresses the user's display/bus/socket. No fake portal.
# Usage: bash tools/linux-portal-consent-tests.sh [--out PARENT]
#        [--revision-stamp FILE] (Windows-authoritative Git metadata for WSL)
# The product archives are built fresh from this tree; no existing build is used.
# Requires existing GNOME/GTK4/AT-SPI/PipeWire/portal and bubblewrap packages.
# Dependencies are never installed by this runner; missing ones fail, not skip.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
if [[ ${1:-} != --session ]]; then
  revision_stamp=
  out="$repo/out/linux-consent"
  while (($#)); do
    case $1 in
      --out) out=${2:?}; shift 2 ;;
      --revision-stamp) revision_stamp=$(realpath "${2:?}"); shift 2 ;;
      *) echo "usage: $0 [--out PARENT] [--revision-stamp FILE]" >&2; exit 64 ;;
    esac
  done
  for tool in bwrap cmake ninja gnome-shell pipewire gdbus dbus-daemon dbus-run-session python3 timeout pkg-config "${CXX:-c++}"; do
    command -v "$tool" >/dev/null || { echo "error: missing $tool" >&2; exit 69; }
  done
  [[ -x /usr/libexec/xdg-desktop-portal && -x /usr/libexec/xdg-desktop-portal-gnome ]] ||
    { echo "error: the real GNOME portal frontend/backend is required" >&2; exit 69; }
  if [[ -z $revision_stamp ]]; then
    command -v git >/dev/null || { echo "error: Git or --revision-stamp is required" >&2; exit 69; }
  fi
  mkdir -p "$out"
  out=$(realpath "$out")
  evidence=$(mktemp -d "$out/consent-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
  sandbox=$(mktemp -d "$HOME/layerc-consent.XXXXXX")
  trap 'rm -rf -- "$sandbox"' EXIT
  mkdir -m700 "$sandbox/runtime" "$sandbox/home" "$sandbox/config" "$sandbox/data" "$sandbox/cache"
  mkdir "$evidence/sources"
  cp -- "$0" "$repo/tools/linux-gui/native_portal_consent.py" "$repo/tools/linux-gui/native_portal_probe.cpp" "$evidence/sources/"
  if [[ -n $revision_stamp ]]; then cp -- "$revision_stamp" "$evidence/source-stamp.json"; fi
  python3 - "$repo" "$evidence" "$revision_stamp" <<'PY'
import hashlib, json, os, re, subprocess, sys
from pathlib import Path
repo, out = map(Path, sys.argv[1:3])
def git(*args):
    return subprocess.check_output(["git", "-C", str(repo), *args])
if sys.argv[3]:
    source = json.loads((out / "source-stamp.json").read_text())
    origin = "caller-provided Windows-authoritative Git stamp"
else:
    source = {
        "gitHead": git("rev-parse", "HEAD").decode().strip(),
        "codeDiffSha256": hashlib.sha256(git("diff", "--binary", "HEAD", "--", "src", "tests", "cmake", "tools")).hexdigest(),
    }
    origin = "Git in source tree"
if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", source.get("gitHead", "")) or not re.fullmatch(r"[0-9a-f]{64}", source.get("codeDiffSha256", "")):
    raise SystemExit("error: malformed source revision stamp")
revision = {
    "gitHead": source["gitHead"],
    "codeDiffSha256": source["codeDiffSha256"],
    "revisionSource": origin,
    "archivedHelpersSha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((out / "sources").iterdir())},
    "evidenceClass": "automated private headless GNOME on Linux/WSL; not physical/VM",
    "outerNamespaces": {n: os.readlink("/proc/self/ns/" + n) for n in ("pid", "mnt", "net", "user")},
    "gnomeVersion": subprocess.check_output(["gnome-shell", "--version"], text=True).strip(),
}
(out / "revision.json").write_text(json.dumps(revision, indent=2))
PY
  # Link only archives compiled here from this tree into a fresh private build
  # directory, so the evidence cannot inherit stale or unrelated objects.
  build="$sandbox/build"
  targets=(layerc_bus layerc_bus_model layerc_linux layerc_capture_common layerc_core layerc_durable)
  { cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DLAYERC_BUILD_TESTS=OFF -DLAYERC_BUILD_EDITOR=OFF &&
    cmake --build "$build" --parallel --target "${targets[@]}"; } > "$evidence/archive-build.log" 2>&1 ||
    { echo "error: building the product archives failed; see $evidence/archive-build.log" >&2; exit 1; }
  archives=()
  for name in "${targets[@]}"; do
    [[ -f "$build/lib$name.a" ]] || { echo "error: missing $build/lib$name.a" >&2; exit 69; }
    archives+=("$build/lib$name.a")
  done
  python3 - "$evidence" "$repo" "${archives[@]}" <<'PY'
import hashlib, json, sys
from pathlib import Path
out, source, archives = Path(sys.argv[1]), sys.argv[2], sys.argv[3:]
(out / "build-provenance.json").write_text(json.dumps({
    "freshBuildFromTree": True,
    "buildSourceDirectory": source,
    "configure": "-G Ninja -DCMAKE_BUILD_TYPE=Debug -DLAYERC_BUILD_TESTS=OFF -DLAYERC_BUILD_EDITOR=OFF",
    "archivesSha256": {Path(a).name: hashlib.sha256(Path(a).read_bytes()).hexdigest() for a in archives},
}, indent=2))
PY
  read -r -a bus_flags <<<"$(pkg-config --cflags --libs libsystemd)"
  # Demand-built TEST-ONLY driver, not installed or linked into any product.
  "${CXX:-c++}" -std=c++20 -Wall -Wextra -Wpedantic -Werror \
    -I "$repo/src/platform/linux" -I "$repo/src/platform/include" \
    -I "$repo/src/core/include" -I "$repo/src/capture/include" \
    "$repo/tools/linux-gui/native_portal_probe.cpp" -Wl,--start-group \
    "${archives[@]}" -Wl,--end-group "${bus_flags[@]}" -lpthread \
    -o "$evidence/native-portal-probe"
  set +e
  bwrap --unshare-user --unshare-pid --unshare-net --die-with-parent \
    --ro-bind / / --tmpfs /run --tmpfs /tmp --dev /dev --proc /proc \
    --ro-bind "$repo" "$repo" --bind "$sandbox" "$sandbox" --bind "$evidence" "$evidence" \
    env -i PATH="$PATH" LANG="${LANG:-C.UTF-8}" \
    HOME="$sandbox/home" XDG_RUNTIME_DIR="$sandbox/runtime" \
    XDG_CONFIG_HOME="$sandbox/config" XDG_DATA_HOME="$sandbox/data" \
    XDG_CACHE_HOME="$sandbox/cache" XDG_CURRENT_DESKTOP=GNOME \
    XDG_SESSION_DESKTOP=gnome XDG_SESSION_TYPE=wayland \
    DBUS_SYSTEM_BUS_ADDRESS="unix:path=$sandbox/no-system-bus" \
    LIBGL_ALWAYS_SOFTWARE=1 GSETTINGS_BACKEND=memory GDK_BACKEND=wayland \
    GTK_A11Y=atspi WAYLAND_DISPLAY=layerc-audit LAYERC_TEST_PRIVATE_NATIVE_SESSION=1 \
    timeout --signal=TERM --kill-after=5s 90s dbus-run-session -- \
    bash "$0" --session "$evidence" > "$evidence/session.log" 2>&1
  rc=$?
  set -e
  python3 - "$evidence" "$rc" <<'PY'
import hashlib, json, os, sys
from pathlib import Path
out, rc = Path(sys.argv[1]), int(sys.argv[2])
cases = []
for name in ("deny", "cancel", "revoke"):
    path = out / name / "result.json"
    cases.append(json.loads(path.read_text()) if path.is_file() else {"case": name, "passed": False})
namespace_file = out / "namespace.json"
ns = json.loads(namespace_file.read_text()) if namespace_file.is_file() else {}
outer = json.loads((out / "revision.json").read_text())["outerNamespaces"]
isolated = bool(ns) and all(ns.get(n) != outer[n] for n in outer)
leftovers = []
for proc in Path("/proc").iterdir():
    if proc.name.isdecimal():
        try:
            if os.readlink(proc / "ns/pid") == ns.get("pid"):
                leftovers.append(int(proc.name))
        except (OSError, PermissionError):
            pass
cleanup = {"joined": True, "leftovers": sorted(leftovers), "privateNamespacesVerified": isolated}
(out / "cleanup.json").write_text(json.dumps(cleanup, indent=2))
passed = rc == 0 and isolated and not leftovers and all(c.get("passed") for c in cases)
summary = {"schemaVersion": 1, "evidenceClass": "automated private headless GNOME on Linux/WSL; not physical/VM", "exitCode": rc,
           "passed": passed, "consentObserved": passed, "cases": cases, "cleanup": cleanup}
(out / "summary.json").write_text(json.dumps(summary, indent=2))
files = sorted(p for p in out.rglob("*") if p.is_file() and p.name != "SHA256SUMS")
(out / "SHA256SUMS").write_text("".join(hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.relative_to(out).as_posix() + "\n" for p in files))
print("evidence=" + str(out))
print(json.dumps(summary))
raise SystemExit(0 if passed else 1)
PY
  exit $?
fi
evidence=$2
# Refuse to write configuration or start services outside the runner's sandbox.
if [[ ${LAYERC_TEST_PRIVATE_NATIVE_SESSION:-} != 1 || -n ${DISPLAY:-} || -n ${AT_SPI_BUS_ADDRESS:-} ||
      ${XDG_RUNTIME_DIR:-} != */layerc-consent.*/runtime ||
      ${XDG_CONFIG_HOME:-} != */layerc-consent.*/config || $(cat /proc/1/comm) != bwrap ]]; then
  echo "error: --session runs only inside the runner's private sandbox" >&2
  exit 64
fi
python3 - "$evidence" <<'PY'
import json, os, sys
from pathlib import Path
(Path(sys.argv[1]) / "namespace.json").write_text(json.dumps({name: os.readlink("/proc/self/ns/" + name) for name in ("pid", "mnt", "net", "user")}, indent=2))
PY
pids=()
cleanup() {
  trap - EXIT INT TERM
  for pid in "${pids[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
  for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT INT TERM
mkdir -p "$XDG_CONFIG_HOME/xdg-desktop-portal"
printf '[preferred]\ndefault=gnome\norg.freedesktop.impl.portal.ScreenCast=gnome\n' > "$XDG_CONFIG_HOME/xdg-desktop-portal/portals.conf"
dbus-daemon --session --nofork --address="$DBUS_SYSTEM_BUS_ADDRESS" > "$evidence/private-system-bus.log" 2>&1 &
pids+=("$!")
pipewire > "$evidence/pipewire.log" 2>&1 &
pids+=("$!")
gnome-shell --wayland --headless --no-x11 --unsafe-mode --virtual-monitor 1280x800 \
  --wayland-display=layerc-audit > "$evidence/gnome-shell.log" 2>&1 &
gnome_pid=$!
pids+=("$gnome_pid")
for ((i=0;i<100;i++)); do
  [[ -S "$XDG_RUNTIME_DIR/layerc-audit" ]] && break
  kill -0 "$gnome_pid" 2>/dev/null || { printf 'GNOME exited before creating the socket\n'; exit 2; }
  sleep .2
done
test -S "$XDG_RUNTIME_DIR/layerc-audit"
export WAYLAND_DISPLAY=layerc-audit
dbus-update-activation-environment HOME XDG_RUNTIME_DIR XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME WAYLAND_DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_DESKTOP XDG_SESSION_TYPE GSETTINGS_BACKEND DBUS_SYSTEM_BUS_ADDRESS
for ((i=0;i<100;i++)); do
  if gdbus introspect --session --dest org.gnome.Mutter.ServiceChannel --object-path /org/gnome/Mutter/ServiceChannel > "$evidence/mutter-service-channel.txt" 2>/dev/null; then break; fi
  kill -0 "$gnome_pid" 2>/dev/null || { printf 'GNOME exited during initialization\n'; exit 3; }
  sleep .2
done
gdbus introspect --session --dest org.gnome.Mutter.ScreenCast --object-path /org/gnome/Mutter/ScreenCast > "$evidence/mutter-screencast.txt"
for ((i=0;i<100;i++)); do
  owner=$(gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.NameHasOwner org.gnome.Shell)
  [[ "$owner" == "(true,)" ]] && break
  kill -0 "$gnome_pid" 2>/dev/null || exit 4
  sleep .2
done
[[ "$owner" == "(true,)" ]]
# Name acquisition precedes GNOME's accepting native GTK clients. Exercise a
# real GTK display connection before launching the graphical portal backend.
gtk_ready=0
for ((i=0;i<60;i++)); do
  if python3 -c "import gi; gi.require_version('Gtk','4.0'); from gi.repository import Gtk; raise SystemExit(0 if Gtk.init_check() else 1)" >> "$evidence/gtk-ready.log" 2>&1; then
    gtk_ready=1
    break
  fi
  sleep .2
done
[[ "$gtk_ready" == 1 ]] || exit 7
# Activate exactly one real backend. Concurrent manual replacement and D-Bus
# activation compete for Mutter's service-channel connection during GTK init.
printf '%s\n' 'Standard real D-Bus activation; backend logs are in session.log.' > "$evidence/portal-gnome.log"
gdbus introspect --session --dest org.freedesktop.impl.portal.desktop.gnome --object-path /org/freedesktop/portal/desktop > "$evidence/gnome-backend-interfaces.txt"
sources=
for ((i=0;i<30;i++)); do
  sources=$(gdbus call --session --dest org.freedesktop.impl.portal.desktop.gnome --object-path /org/freedesktop/portal/desktop --method org.freedesktop.DBus.Properties.Get org.freedesktop.impl.portal.ScreenCast AvailableSourceTypes 2>"$evidence/gnome-sources.stderr" || true)
  [[ "$sources" =~ uint32[[:space:]]+[1-7] ]] && break
  sleep .2
done
printf '%s\n' "$sources" > "$evidence/gnome-source-types.txt"
[[ "$sources" =~ uint32[[:space:]]+[1-7] ]] || { printf 'Real backend never advertised capture sources\n'; exit 5; }
/usr/libexec/xdg-desktop-portal --verbose --replace > "$evidence/portal.log" 2>&1 &
pids+=("$!")
for ((i=0;i<100;i++)); do
  sources=$(gdbus call --session --dest org.freedesktop.portal.Desktop --object-path /org/freedesktop/portal/desktop --method org.freedesktop.DBus.Properties.Get org.freedesktop.portal.ScreenCast AvailableSourceTypes 2>/dev/null || true)
  [[ "$sources" =~ uint32[[:space:]]+[1-7] ]] && break
  sleep .2
done
printf '%s\n' "$sources" > "$evidence/frontend-source-types.txt"
[[ "$sources" =~ uint32[[:space:]]+[1-7] ]] || { printf 'Real frontend never advertised capture sources\n'; exit 6; }
for ((i=0;i<30;i++)); do
  if gdbus introspect --session --dest org.freedesktop.portal.Desktop --object-path /org/freedesktop/portal/desktop > "$evidence/portal-interfaces.txt" 2> "$evidence/portal-introspect.stderr"; then break; fi
  sleep .2
done
python3 - "$evidence/portal-interfaces.txt" <<'PY'
import sys
from pathlib import Path
assert 'interface org.freedesktop.portal.ScreenCast {' in Path(sys.argv[1]).read_text(), 'real ScreenCast interface is not available'
PY
python3 "$repo/tools/linux-gui/native_portal_consent.py" --fixture > "$evidence/fixture.log" 2>&1 &
pids+=("$!")
sleep 1
for case in deny cancel revoke; do
  case_root="$evidence/$case"
  mkdir -m700 "$case_root"
  "$evidence/native-portal-probe" "$case_root" > "$case_root/client.log" 2>&1 &
  client_pid=$!
  pids+=("$client_pid")
  python3 "$repo/tools/linux-gui/native_portal_consent.py" "$case_root" "$case" > "$case_root/ui-controller.log" 2>&1
  wait "$client_pid"
  unset 'pids[${#pids[@]}-1]'
done
printf '%s\n' 'Real native deny/cancel/revoke probes completed; inspect per-case results.'
