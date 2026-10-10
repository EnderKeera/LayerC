# LayerC on Linux (V2.6)

This document owns Linux behavior, commands, packaging, permissions, and
physical evidence. Use [`../README.md`](../README.md) for the project-wide
release summary and current source/CMake/workflows for exact implementation and
gate details.

## Support status

The daemon and CLIs shipped in the `v2.6.0` release as **source/CLI only**.
v2.6c (package version 2.6.2) adds the X11 `layere` editor, a native Debian
package and an editor Flatpak; it is built and gated locally but not yet
published, so build from source as described below. The physical GNOME
Xorg/KDE X11 evidence below is **pending**: it has not been archived. CI
verifies headless, Xvfb, installed-native, package and Flatpak runtime
behavior, but it is not a substitute for the hardware checklist.

| Surface | State | Boundary |
| --- | --- | --- |
| V2.6.0 portable runtime | implemented | Foreground `layerd`, `layerctl`, XDG storage, Unix-socket control, bundles, install metadata |
| V2.6.1 X11 interaction | implemented | EWMH tracking, passive XI2 input, Shape overlays, RandR scaling |
| V2.6.2 capture/calibration | implemented | Composite/Damage capture, bounded fallback, `layercal` PPM/JSON output |
| v2.6c X11 editor | first-release feature set; advanced parity in progress | FLTK `layere`: profiles, matching, cooldowns, bindings, panels, import/export, accessibility; installed, `.deb` and Flatpak gates; not yet published |
| Physical native/Flatpak evidence | pending | GNOME Xorg and KDE Plasma X11 runs must be archived |
| Gamescope and Wayland | unsupported in V2.6 | No compatibility promise or silent fallback |

On X11, `layerd` reads passive input and paints click-through overlays; the
shared capture adapter also backs bounded CLI snapshots. Without `$DISPLAY`,
the daemon remains a headless store/control bridge and publishes graphical
capabilities as unavailable; `layercal` fails visibly. Name matching on both
platforms uses the shared deterministic fold (ASCII plus the cased Latin-1
supplement rows; all other codepoints compare byte-exact), replacing the
former Windows `LCMapStringEx` invariant mapping so Windows and Linux agree.

## X11 session (V2.6.1)

`layerd` opens one X11 connection (`$DISPLAY`) shared by every adapter and
registers its socket on the event loop:

| Adapter | Status | Notes |
| --- | --- | --- |
| Connection + extension probes | done | `layerctl status --json` publishes XI2/Shape/RandR/Composite/Damage/XFixes versions, EWMH availability, service state, capability availability, stable reason tokens, and bounded current session fault diagnostics |
| Window tracking | done | EWMH `_NET_CLIENT_LIST` with a bounded root-child fallback; client title/class/PID/state and structure changes, plus root-child creation/map/unmap/destruction, invalidate discovery through the shared input/event dispatcher without periodic rescans; `GameMatch` uses the shared conjunctive rule (every specified field must hold, case-folded, byte-exact executable path), preferring the active window, then the previously followed one |
| Input | done | XI2 raw key/button events plus hierarchy tracking for hotplug |
| Overlay | done | One click-through window per panel; Shape is required, otherwise the overlay stays hidden |
| Capture | done | Composite named pixmaps + Damage coalescing, with a 100 ms bounded direct-window fallback; exposed by `layercal` |

Input is **passive only**: events are read, never synthesized, and XTest is
linked into the test binary alone — never into `layerd`. The current server's
XKB physical key names and aliases are translated into the existing Windows
scan-code tokens, independent of layout symbols or a fixed keycode offset:
physical E (`AD03`) is `0x12`, left Alt is `0x38`, and right Control is
`0xE01D`. Left/right modifiers and extended navigation keys remain distinct;
unknown physical names leave their bindings inactive without rewriting profiles.
XKB map/name changes refresh the translation. Pointer buttons follow the
Windows daemon's numbering: conventional X middle/right buttons 2/3 map to
profile 3/2, wheel buttons 4/5 to 6/7, and back/forward buttons 8/9 to 4/5.
Horizontal wheel buttons 6/7 are unsupported, not aliases for side buttons.

Gamepads are the gamepad-class devices XI2 exposes (buttons plus at least two
axes, no scroll class, no pointer-like name); when none is visible the daemon
logs that controller bindings stay inactive instead of failing silently.
Unclassified button-only devices are ignored rather than treated as mice.

### Overlays

One override-redirect X11 window per panel, sized and anchored exactly like
the Windows overlay (bar metrics are authored at 96 dpi and scaled per
monitor). Each window is shaped to an **empty ShapeInput region**, so a click
passes through to the window below; the region is queried back after it is set
and an overlay that would intercept input is never shown. Without the Shape
extension the overlay stays hidden and the daemon publishes
`overlay: shape extension not advertised by the X server` instead.

Overlays are painted with plain rectangles — no compositor, no toolkit. Bars
fill from the left and drain towards ready; a bar panel also honours
`hideWhenReady` and unmaps itself. Only active sessions get overlay targets.
Automation can force a ready bar visible, hide any panel, or replace its text.
Visibility and bar text come from the shared core panel presentation, so
Windows, Linux, and the Game Bar companion apply one rule.
While an overlay is mapped the daemon arms a 100 ms repaint deadline and
nothing else: once every panel is hidden or no session is active, the loop
blocks again.

The monitor scale comes from RandR physical size (clamped to 72–288 dpi, 96
when a server reports nothing usable), matching the Windows `MulDiv`-style
scaling so one profile looks the same on both platforms.

Smoke test on a headless box:

```sh
Xvfb :99 -screen 0 1280x720x24 &        # any X server works
DISPLAY=:99 layerd                      # logs sessions, capability reasons, triggers
layerctl status --json                  # archive capability + extension snapshot
```

### V2.6.1 hardware evidence checklist

Archive these release-evidence runs on a physical Linux desktop; they do not
block implementation or container CI:

- Native and Flatpak runs on GNOME Xorg and KDE Plasma X11.
- `layerctl status --json` before/after profile activation, including X11
  versions, capability availability, service state, and reason tokens.
- Passive keyboard and mouse triggers plus real hierarchy hotplug; confirm no
  input is blocked or synthesized.
- Executable, title, and class matching, including the documented
  Flatpak case where `/proc/<pid>/exe` is unreadable and only title/class
  profiles match.
- Overlay visibility and click-through with a real pointer, including HiDPI
  panels and the missing-Shape hidden-overlay failure mode.
- Headless/non-X11 and missing-extension snapshots showing the affected
  adapter as unavailable rather than silently omitted.

## X11 editor (v2.6c)

The v2.6c editor is not yet published and not yet a full Windows editor
replacement (advanced modules follow). When X11 development libraries, FLTK 1.4+ with
`fltk-config`, and ATK, its AT-SPI adaptor and libatspi 2.62 or later are
available, `LAYERC_BUILD_EDITOR=ON` (the default) adds the `layere`
target. Older ATK/AT-SPI adaptors crash during cleanup, so 2.62 is the
floor: Debian 13 takes 2.62 from `trixie-backports` (its main archive has
2.56), while Ubuntu 24.04 (2.52) does not qualify. `tools/linux-editor-deps.txt`
lists the editor's Debian packages and their suite. Missing dependencies leave the existing daemon/CLI and headless build
graph usable and do not count as passing an editor gate. Configure with
`-DLAYERC_REQUIRE_EDITOR=ON` to make them a configuration error instead.
FLTK and ATK are linked into `layere` only, not into `layerd` or the shared
core. GTK widgets are not used.

```sh
cmake --build build-linux --target layere
./build-linux/layere
```

An X11 display is required. The editor reads the normal XDG profile store.
It creates, duplicates, renames, enables/disables and deletes profiles, and
edits game matching, cooldowns, keyboard and mouse bindings (with capture), and
panels. It has undo/redo, menus and context menus, and JSON/`.layercprofile`
import with collision review and export. Its preview is illustrative, not a
complete renderer. Advanced modules (automation, visual, calibration,
companion), Generic HID bindings and cooldown groups are preserved but not
editable; unreadable/invalid module data blocks Save rather than being
silently discarded. Save uses the shared durable
stage/commit path, then asks the daemon to reload through the authenticated
private control socket.

LayerE exports its windows, menus and controls to screen readers through ATK
and the AT-SPI bus without GTK widgets. When the bus or a coherent ≥ 2.62
library set is unavailable, the window shows the reason and editing continues.

Invalid inputs and storage failures retain unsaved edits and show a reason.
A successful offline save is distinguished from a save whose daemon reload
request was acknowledged or could not be confirmed. An acknowledgement does
not prove completed runtime application, nor does a lost acknowledgement prove
that no reload occurred. Inspection/permission faults remain visible rather
than being misreported as an offline daemon. The editor does not start or stop
the daemon.
Closing or selecting another profile prompts to save, discard or cancel dirty
edits. Daemon capabilities/faults are visible, with an explicit refresh button.

The installed `layere` is smoke-tested by `tools/linux-editor-smoke.sh`: it
starts beside the installed daemon on a disposable display with private XDG
roots, must map its window, stay within the 20 MiB idle USS and 0.2% idle
CPU targets, and quit through Ctrl+Q. Advanced editor parity is not
complete. Xvfb walkthrough evidence is automated evidence, not physical
GNOME/KDE coverage or a screen-reader user study.
Gamescope and native Wayland remain unsupported in V2.6.

### GUI walkthrough runner and agent bridge

`tools/linux-editor-gui-tests.sh` runs the editor in a disposable session: a
private authenticated Xvfb display (1600x1000), Openbox, a private D-Bus
session, private HOME/XDG roots, the X11 smoke fixture as the game, `layerd`
and `layere`. It never addresses the user's display, bus, daemon, control
socket or profiles. Every helper is owned, stopped and joined; a missing
dependency or build output fails the run rather than skipping it.

```sh
tools/linux-editor-gui-tests.sh --build build-linux --out out/linux-gui/manual
```

The default `walkthrough` mode drives `tools/linux-gui/walkthrough.py`:
bridge guard rejections; startup; invalid input left unwritten; edit, durable
save, acknowledged reload and a moved live overlay; frame-corner resize and
minimum-size clamp; offline save; and a visible store failure that leaves the
original profile intact. A failed case blocks the later cases; it never passes
them. Menus, CRUD, import/export, keyboard/accessibility, maximize/scroll/font
scaling and daemon reconnect are recorded as deferred to the full editor.

`--mode session` keeps the computer-use bridge (`tools/linux-gui/bridge.py`)
open for one agent instead. It listens on a private Unix socket, requires the
session token from the same user, and acts only on the runner-owned display:
it pins that Xvfb through a pidfd, marks its root window with a session nonce,
and re-confirms both before every X command. Losing the display ends the
session and fails the run. Each click, drag, key or text action must cite the
most recent screenshot, and that screenshot must postdate the previous action;
anything else is rejected and logged. Direct window geometry and the test-only fixture faults (store
block/restore, daemon stop) are logged as distinct action kinds. Operations:
`screenshot`, `inspect` (region digest/colour centroid), `state` (profile
bytes plus daemon status), `tree` (AT-SPI), `a11y_check` (semantic AT-SPI walkthrough),
`click`, `drag`, `set_text`, `key`, `type`, `activate`, `set_geometry`,
`block_profile`, `restore_profile`, `daemon_stop` and `stop`.

From Windows (PowerShell 7.3+), `tools/test-linux-gui.ps1` records the
source revision (HEAD, dirty state and a digest of uncommitted changes),
mirrors the checkout into a dedicated Linux directory, builds the editor
targets in Release, re-checks that the working tree did not change during the
copy, and runs the helper. The mirror directory must be new, empty or already
marked as a runner mirror; home, the checkout and overlapping build paths are
refused. One run or session uses a mirror/build pair at a time:

```powershell
tools/test-linux-gui.ps1 -EnvScript '~/dev-env.sh'          # scripted walkthrough
tools/test-linux-gui.ps1 -Mode Resources                         # representative store
tools/test-linux-gui.ps1 -Mode Session -EnvScript '~/dev-env.sh'  # agent session
tools/test-linux-gui.ps1 -Mode Call -Request '{"op":"screenshot","name":"start"}'
tools/test-linux-gui.ps1 -Mode Stop
```

Each run writes `out/linux-gui/<run>/`: `revision.json`, executable checksums,
tool versions, `actions.jsonl`, `shots/`, `cases.json` (walkthrough), the
profile before/after, editor idle/startup memory and CPU, a separate harness
process snapshot, owned-process exit records, `cleanup.json`, `result.json` and
`SHA256SUMS`. The final atomic `runner-complete.json` binds the result and
manifest hashes after cleanup/report generation. Windows `Stop` waits for and
validates that publication before releasing the active-session slot; incomplete
or corrupt publication retains the slot and evidence for retry. The result
passes only if every required case passed, LayerE met its idle/startup USS and
idle CPU targets, and cleanup left nothing behind. `Resources` mode loads the
representative 20-profile store, applies the 32 MiB USS targets and runs 100
profile-switch/dialog cycles that must stay within 2 MiB without a rising trend
(`cycles.json`).

## V2.6.2 calibration and evidence

`layercal` is intentionally a CLI, not a second editor. Select an X11 window
with `xwininfo`, then copy its decimal or `0x` XID:

```sh
layercal snapshot 0x4600007 --quality balanced > snapshot.ppm
layercal snapshot 0x4600007 --quality balanced \
    --region 0.25 0.20 0.50 0.40 --json > snapshot.json
```

The first form writes only binary P6 PPM bytes to stdout. The JSON form writes
sampled dimensions, backend/availability, the validated normalized region and
its pixel bounds, expected PPM byte count, live capture resource counts, and a
fault diagnostic. Low and Balanced bound the longest sampled axis to 64 and
256 pixels respectively; `cacheBytes` is exactly `width * height * 3`.
Regions must be finite, nonempty, and wholly inside 0..1. Diagnostics never
share stdout with snapshot bytes, so host-shell redirection is safe.

Archive the following **physical** release evidence on both GNOME Xorg and KDE
Plasma X11, native and Flatpak. Xvfb/WSL proves deterministic behavior but is
not a substitute, and this document deliberately records no fabricated pass:

- `layerctl status --json`, including Composite 0.2, Damage 1.0, and XFixes 2.0.
- Low and Balanced PPM/JSON pairs from a controlled colored window; verify the
  axis bounds, region pixel bounds, `captureSessions: 1`, and bounded cache.
- Composite capture under the desktop compositor and the documented direct
  fallback on an unredirected test window; resize and destroy the target and
  archive the resulting nonzero exit/fault JSON.
- A 30-frame black/protected policy run plus recovery, using the focused X11
  capture test, and process RSS/CPU/file-descriptor observations while
  repeatedly sampling Low and Balanced.
- Confirm `captureSessions`, owned objects, and cache return to zero after the
  process/test session exits, with no X server grab and no idle polling.

Physical evidence is still **pending** until those artifacts are archived;
container CI and the focused Xvfb suite cover implementation, limits, and
fault cleanup only. On each physical desktop, choose a controlled target with
`xwininfo`, then run both package modes:

```sh
bash tools/run-linux-v26-evidence.sh gnome native 0x4600007   # LAYERC_BUILD_DIR defaults to build-linux
bash tools/run-linux-v26-evidence.sh gnome flatpak 0x4600007
bash tools/run-linux-v26-evidence.sh kde native 0x4600007
bash tools/run-linux-v26-evidence.sh kde flatpak 0x4600007
```

The harness refuses non-X11 sessions, never synthesizes input, records the
status/capture/resource artifacts under `out/benchmarks/v26-linux/`, and
requires an explicit operator pass for the visibility, click-through, hotplug,
recovery, and visual-transition checks that cannot be proved automatically
(prompted, or preset with `LAYERC_MANUAL_RESULT=pass|fail` and
`LAYERC_MANUAL_NOTES`; unset without a terminal records `pending`).
Set `LAYERC_RUN_AUTOMATION=1` to run and bind the reusable automated gate before
the physical checklist, or point `LAYERC_AUTOMATION_SUMMARY` at an existing
commit-matched `summary.json`. Release collection can set
`LAYERC_REQUIRE_AUTOMATION=1`; this rejects an unbound physical run and
requires clean automated and physical source trees. It still never marks any
manual observation as passed.

## Building

Requires a C++20 compiler, CMake 3.28+, Ninja (or make). The full X11/Xvfb
path on Debian or Ubuntu uses:

```sh
xargs -a tools/linux-deps.txt sudo apt-get install --no-install-recommends
```

`libxtst-dev` is test-only; without the other X11 development packages LayerC
builds the headless daemon and reports graphical capabilities as unavailable.
From the repo root:

```sh
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure   # portable + Linux suites
```

Linux builds `layerd`, `layerctl`, and `layercal` when the required X11
libraries are present. FLTK 1.4+ and ATK/AT-SPI 2.62+ development files
additionally enable the X11 `layere` editor described above.
Missing editor dependencies do not disable
the existing headless/source targets. The isolated capture worker and Game Bar
host remain Windows-only.

### Complete automated gate

`tools/run-linux-tests.sh` is the reusable gate used by CI and physical-evidence
preflight. It keeps generated state in explicit build/evidence directories:

```sh
bash tools/run-linux-tests.sh \
    --build-dir build/linux-automation \
    --evidence-dir out/linux-automation/local \
    --flatpak auto --editor auto --package off
```

The native stages use private XDG roots. The complete CI gate runs on
disposable workers and performs the following:

- configures, builds, installs, and runs the portable/core/Linux suites with no
  display;
- runs the five required X11 tracking, input, overlay, capture, and visual
  suites under Xvfb, failing if one is absent or skipped;
- validates both stages against the `linux-headless` and `linux-x11` suite
  lists in `tools/test-gates.json`, so a suite that is not registered (for
  example because a dependency such as `libxtst-dev` is missing) fails the
  gate instead of shrinking it;
- starts the installed `layerd`, drives installed `layerctl`, and exercises
  installed `layercal` against a controlled Xvfb fixture window using private
  `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, and `XDG_RUNTIME_DIR`;
- records CTest JUnit, registration and validation summaries, daemon logs,
  status snapshots, PPM/JSON captures, process resources, environment/tool
  versions, the exact Git commit and dirty state, plus SHA-256 checksums;
- with `--editor auto|required` (default `auto`), runs the CTest `editor` gate
  (`linux-editor` in `tools/test-gates.json`) under Xvfb and the installed
  editor smoke. `required` configures with `LAYERC_REQUIRE_EDITOR=ON`, so a
  missing FLTK/ATK dependency fails instead of shrinking coverage; `off`
  records `not-run`;
- with `--package auto|required` (default `off`), builds a separate
  `/usr`-prefixed Release tree, creates the Debian package with CPack and
  checks it with `tools/linux_package_check.py` (metadata, contents, desktop
  entry, AppStream). Only as root inside a container its launcher declared
  disposable (`LAYERC_DISPOSABLE_ROOT=1`, passed by `tools/ci-linux.sh`) does
  it install the `.deb` with apt, run the headless, X11 and editor smokes
  against `/usr/bin`, remove the package and confirm that it and its files are
  gone; elsewhere installation is recorded as unavailable. A required package needs
  a required editor; with `--editor off`, `--package auto` records the package
  as unavailable;
- builds, installs, and runtime-smokes the Flatpak when `flatpak-builder` and
  the 25.08 Platform/SDK are installed. The runner redirects its user install
  and HOME/XDG paths; this is best-effort on a shared host, so use a disposable
  user or CI worker for Flatpak evidence. Only a system-wide 25.08 runtime
  counts, because the build uses a private user installation. Flatpak staging
  lives beside the build directory (`<build-dir>-flatpak.*`; inside the
  checkout only under `build/`, which the manifest does not copy) and is
  removed after a successful stage. Unless `--editor off`, the Flatpak stage
  also runs the editor smoke; it always exports the bundle to
  `<evidence-dir>/packages/LayerC.flatpak`. `auto` records unavailable coverage explicitly;
  `required` fails instead. `LAYERC_RELEASE_EVIDENCE=1` requires the Flatpak,
  editor and package stages and a clean source tree before and after the gate.

`tools/ci-linux.sh <image> [evidence-directory] [options...]` runs the same
gate in a read-only source mount, passing the options to the gate. The
container is the disposable root: `tools/linux-gate-in-root.sh` installs
`tools/linux-deps.txt` and, unless `--editor off`, the editor packages, then
runs the gate with package installation allowed; it refuses to run outside a
container. Its build is container-local while the evidence mount survives
container teardown. An `off` Flatpak policy is recorded as `not-run`,
never as a pass; the dedicated Ubuntu Flatpak CI job supplies required coverage.
That job passes `--flatpak-only` (valid only with `--flatpak required` and
without release evidence): it builds just the smoke fixture natively, records
the native stage as `not-run`, and runs the Flatpak build and smokes, since the
container jobs already cover the native gate.

## Storage and IPC paths

Per the [XDG Base Directory spec](https://specifications.freedesktop.org/basedir/latest/):

| Path | Purpose |
| --- | --- |
| `$XDG_CONFIG_HOME/layerc/profiles/<id>.json` | profile JSON (default `~/.config`), plus a `<id>.json.bak` of the previous version after a replace |
| `$XDG_DATA_HOME/layerc/profiles/<id>/` | profile sidecars/state (default `~/.local/share`) |
| `$XDG_DATA_HOME/layerc/store/` | journal, backups, writer lock |
| `$XDG_RUNTIME_DIR/layerc/control.sock` | daemon control socket (fallback `/tmp/layerc-<uid>/`, mode 0700, with a warning) |

Relative `XDG_*` values are ignored in favor of the absolute fallbacks.
Import a Windows profile or bundle with `layerctl` (JSON/`.layercprofile` is
the only cross-platform transfer path; there is no directory migration).

## Usage

```sh
layerd                       # foreground daemon; SIGINT/SIGTERM shut down
layerctl status [--json]     # summary: sessions and capability availability
layerctl reload
layerctl stop
layerctl profile import-json < profile.json
layerctl profile export-json <id> > profile.json
layerctl bundle import [--keep-existing] [--dry-run] < profiles.layercprofile
layerctl bundle export <id...> > profiles.layercprofile
layercal snapshot <x11-window-id> [--quality low|balanced|exact] > snapshot.ppm
layercal snapshot <x11-window-id> [--region x y width height] --json > snapshot.json
                             # region: normalized 0..1 fractions, recorded in the JSON metadata
```

Imports read stdin and publish transactionally; a rejected profile names the
failing JSON field. The store file name is the sanitized id; an id whose
sanitized name is already taken by another profile gets a `-2`, `-3`, ...
suffix instead of colliding. Exports write only payload bytes to stdout
(diagnostics go to stderr). Bundle import replaces colliding
profiles by default and prints each decision; `--keep-existing` skips
collisions instead (an all-skipped import succeeds without a transaction), and
`--dry-run` prints the plan without writing.
Journal backups and directory ancestors are flushed before publication. If a
publication or commit-marker directory flush remains unconfirmed, the operation
reports failure and retains recovery evidence for the next recovery attempt.
Removed module sidecars retain their bytes behind an authoritative tombstone;
cleanup never deletes sidecars after releasing writer ownership.
Unparseable transaction journals are quarantined as `<name>.corrupt-<n>` for
operator inspection rather than deleted.
Import/export/reload commands work offline; after an import a running daemon
is notified when its socket exists. One daemon per socket — a live daemon
refuses a second bind. Both socket endpoints authenticate the peer's effective
UID; clients reject a reachable foreign-UID listener before sending a request.
A control command or reload notification succeeds only after accepting a valid,
successful `LayerC.Control` v1 response, including with `--json`; accepted JSON
retains the original response bytes. Daemon replies use bounded, expiring queues
advanced by writable readiness on the shared event loop, so a slow reader does
not block other clients, passive input, or deadlines.

Foreground loss or XI2 detachment clears transient held-key state while matched
sessions retain cooldowns and automation. When several windows match, a session
keeps its previous target in the background and prefers an active match on focus
gain. `layerctl stop`, SIGINT, and SIGTERM
run session-stopping automation and checkpoint persistent effects before the
sessions are destroyed; temporary capability quiescence only checkpoints.

## Install layout (GNUInstallDirs)

```sh
cmake --install build-linux --prefix /usr/local  # BINDIR: layerd, layerctl, layercal, layere
```

With the editor, the install also places the `io.github.EnderKeera.LayerC`
desktop entry, hicolor icons (48–256 px) and a desktop-application metainfo;
without it, only the console-application metainfo is installed.

### Debian package

```sh
cmake -S . -B build-deb -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr -DLAYERC_REQUIRE_EDITOR=ON
cmake --build build-deb --parallel
cpack --config build-deb/CPackConfig.cmake -B build-deb/cpack   # layerc_2.6.2-1_amd64.deb
```

CPack derives the library dependencies with `dpkg-shlibdeps` (needs
`dpkg-dev` and `file`) and adds the ATK, ATK bridge and AT-SPI ≥ 2.62 floor.
Debian 13's main archive has 2.56, so enable `trixie-backports` and install
from it explicitly:

```sh
sudo apt install -t trixie-backports ./layerc_2.6.2-1_amd64.deb
```

One `layerc` package carries the daemon, CLIs and editor. It also installs
the systemd user unit described below with `ExecStart=/usr/bin/layerd`, but
never enables or starts it.

### Optional systemd user unit (default OFF)

```sh
cmake -S . -B build-linux -DLAYERC_INSTALL_SYSTEMD_USER_UNIT=ON -DCMAKE_INSTALL_PREFIX=~/.local
cmake --build build-linux --parallel
cmake --install build-linux
systemctl --user start io.github.EnderKeera.LayerC.service   # never auto-enabled
```

The unit (`packaging/linux/io.github.EnderKeera.LayerC.service.in`) is
`Type=simple` with a foreground `ExecStart` baked from the installed bindir
and `UMask=0077`. Direct launch remains the baseline and behaves equivalently.

## Flatpak

Manifest: `packaging/flatpak/io.github.EnderKeera.LayerC.json`
(runtime `org.freedesktop.Platform` 25.08). The runtime has no FLTK and only
AT-SPI 2.54, so the manifest builds FLTK 1.4.3 (static) and at-spi2-core
2.62.0 from checksum-pinned upstream archives before LayerC; the editor is
required there. Its default command is `layere`, and it installs the desktop
entry, icons and desktop-application metainfo. Every Linux install, including the Flatpak, carries the notices for the
statically linked miniz under `share/licenses/LayerC/`
(`THIRD_PARTY_NOTICES.md`, `miniz/LICENSE`, `miniz/UPSTREAM.md`); the automated
gate fails if the native or Flatpak install tree lacks them.

The metainfo uses `LicenseRef-proprietary` because the repository has no
project-level license; replace it only with an owner-approved SPDX license.
Authenticated repository access confirms control of the `EnderKeera/LayerC`
namespace, but the code-hosting URL must also be publicly reachable before a
Flathub submission. The manifest has not been submitted and builds its
bundled FLTK and AT-SPI from source, so it is for local or self-hosted Flatpak
repositories until a publishing channel accepts it.

The manifest grants `--socket=x11`, which is required for the one Xlib
connection used by XI2, Shape, RandR, EWMH and the editor window, and v2.6c
adds `--filesystem=xdg-documents` and `--filesystem=xdg-download` so the
editor's import/export chooser can reach the usual exchange folders. It does
**not** grant network, device, home or host filesystem, Wayland, session-bus,
IPC-share, or background access; the screen-reader bridge uses the
accessibility bus Flatpak provides to every app. X11 access is intentionally a
separately reviewed permission because an X server does not isolate clients;
removing it returns the Flatpak to the V2.6.0 store/control-only mode, which
also disables the editor (the default command), so run the daemon and CLIs
with `--command=`. Remove the two folders to keep imports and exports to shell
redirection.

```sh
flatpak-builder --user --install --force-clean build-flatpak \
    packaging/flatpak/io.github.EnderKeera.LayerC.json

flatpak run io.github.EnderKeera.LayerC                              # editor
flatpak run --command=layerd io.github.EnderKeera.LayerC             # daemon (fg)
flatpak run --command=layerctl io.github.EnderKeera.LayerC status --json
flatpak run --command=layerctl io.github.EnderKeera.LayerC bundle export a b > out.layercprofile
flatpak run --command=layercal io.github.EnderKeera.LayerC snapshot 0x4600007 --quality low > low.ppm
flatpak run --command=layercal io.github.EnderKeera.LayerC snapshot 0x4600007 --json > capture.json
```

The sandbox keeps private XDG roots under `~/.var/app/io.github.EnderKeera.LayerC`
and its socket under the per-app `XDG_RUNTIME_DIR` — intentionally separate
from the native store/socket. Host shell redirection is the file bridge in
and out of the sandbox. Flatpak PID isolation can hide host executable paths;
a profile that specifies `executableName` or `executablePath` then does not
match (fields are conjunctive and never fall back), so Flatpak-only profiles
should identify the game by `windowClass`/`windowTitle`.

CI builds the manifest and runs daemon/control/X11 and editor smokes under
Xvfb. A physical
desktop is still required for real compositor, pointer, monitor, and GPU
observations; automated Flatpak success does not satisfy that checklist.

## CI and release boundary

The `linux` (`debian:stable`, `ubuntu:24.04`) and `linux-flatpak` jobs in
`ci.yml` run the gate above and are the automated Linux compatibility gate;
job details are in
[`../CONTRIBUTING.md`](../CONTRIBUTING.md#ci-and-release-gates). The
`debian:stable` job runs `--editor required --package required`; `ubuntu:24.04`
(AT-SPI below 2.62) runs `--editor off --package off`, keeping the source and
headless build without the editor. No Linux package is published yet:
`windows-msvc` produces the only release ZIP, and the `.deb` and
`LayerC.flatpak` exist only as gate evidence. Physical
input/click-through, real-monitor/driver behavior, and GNOME Xorg/KDE X11
native plus Flatpak evidence remain manual and pending until archived.
Future Gamescope or Wayland lanes could reuse the evidence schema, but no such
backend or claim is part of V2.6.
