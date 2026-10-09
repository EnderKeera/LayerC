# LayerC on Linux (V2.6)

This document owns Linux behavior, commands, packaging, permissions, and
physical evidence. Use [`../README.md`](../README.md) for the project-wide
release summary and current source/CMake/workflows for exact implementation and
gate details.

## Support status

Linux V2.6 is part of the `v2.6.0` release as **source/CLI only**. No Linux
package or Flatpak bundle is published; build it from source as described
below. The physical GNOME Xorg/KDE X11 evidence below is **pending**: it has not
been archived. CI verifies headless, Xvfb, installed-native, and Flatpak
runtime behavior, but it is not a substitute for the hardware checklist.

| Surface | State | Boundary |
| --- | --- | --- |
| V2.6.0 portable runtime | implemented | Foreground `layerd`, `layerctl`, XDG storage, Unix-socket control, bundles, install metadata |
| V2.6.1 X11 interaction | implemented | EWMH tracking, passive XI2 input, Shape overlays, RandR scaling |
| V2.6.2 capture/calibration | implemented | Composite/Damage capture, bounded fallback, `layercal` PPM/JSON output |
| v2.6c editor slice | experimental | FLTK X11 profile/first-cooldown/first-panel editing, durable Save and daemon status/reload; not installed or packaged yet |
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

## Experimental X11 editor slice

The v2.6c editor is in development, not a published release or a full Windows
editor replacement. When X11 development libraries, FLTK 1.4+ and
`fltk-config` are available, `LAYERC_BUILD_EDITOR=ON` (the default) adds the
source-build `layere` target. Missing FLTK leaves the existing daemon/CLI and
headless build graph usable; it does not count as passing an editor gate.
FLTK is linked into `layere` only, not into `layerd` or the shared core.

```sh
cmake --build build-linux --target layere
./build-linux/layere
```

An X11 display is required. The slice reads the normal XDG profile store and
edits profile name, executable match and enabled state plus the first cooldown
and first panel's normalized position. Its preview is illustrative, not a
complete renderer. Existing bindings, other panels and valid module sidecars
are retained; unreadable/invalid module data blocks Save rather than being
silently discarded. Save uses the shared durable stage/commit path, then asks
the daemon to reload through the authenticated private control socket.

Invalid inputs and storage failures retain unsaved edits and show a reason.
A successful offline save is distinguished from a save whose daemon reload
request was acknowledged or could not be confirmed. An acknowledgement does
not prove completed runtime application, nor does a lost acknowledgement prove
that no reload occurred. Inspection/permission faults remain visible rather
than being misreported as an offline daemon. The editor does not start or stop
the daemon.
Closing or selecting another profile prompts to save, discard or cancel dirty
edits. Daemon capabilities/faults are visible, with an explicit refresh button.

CRUD, import/export, complete accessibility, advanced editor parity, installed
smoke, GUI CI and editor packaging are not complete in this slice. Xvfb
walkthrough evidence is automated evidence, not physical GNOME/KDE coverage.
Gamescope and native Wayland remain unsupported in V2.6.

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
libraries are present. FLTK 1.4+ development files additionally enable the
experimental X11 `layere` slice described below. Missing FLTK does not disable
the existing headless/source targets. The isolated capture worker and Game Bar
host remain Windows-only.

### Complete automated gate

`tools/run-linux-tests.sh` is the reusable gate used by CI and physical-evidence
preflight. It keeps generated state in explicit build/evidence directories:

```sh
bash tools/run-linux-tests.sh \
    --build-dir build/linux-automation \
    --evidence-dir out/linux-automation/local \
    --flatpak auto
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
- builds, installs, and runtime-smokes the Flatpak when `flatpak-builder` and
  the 25.08 Platform/SDK are installed. The runner redirects its user install
  and HOME/XDG paths; this is best-effort on a shared host, so use a disposable
  user or CI worker for Flatpak evidence. Flatpak staging stays under
  `build/flatpak-automation.*`, which the manifest excludes from its source copy.
  `auto` records unavailable coverage explicitly; `required` (also selected by
  `LAYERC_RELEASE_EVIDENCE=1`) fails instead. Release mode also requires a clean
  source tree before and after the gate.

`tools/ci-linux.sh <image> [evidence-directory]` runs the same gate in a
read-only source mount. Its build is container-local while the evidence mount
survives container teardown. An `off` Flatpak policy is recorded as `not-run`,
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
cmake --install build-linux --prefix /usr/local  # BINDIR: layerd, layerctl, layercal
```

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
(runtime `org.freedesktop.Platform` 25.08). GNUInstallDirs also installs
`io.github.EnderKeera.LayerC.metainfo.xml` as a console application. V2.6 has no
graphical launcher, so it intentionally installs no desktop entry or icon.
Every Linux install, including the Flatpak, carries the notices for the
statically linked miniz under `share/licenses/LayerC/`
(`THIRD_PARTY_NOTICES.md`, `miniz/LICENSE`, `miniz/UPSTREAM.md`); the automated
gate fails if the native or Flatpak install tree lacks them.

The metainfo uses `LicenseRef-proprietary` because the repository has no
project-level license; replace it only with an owner-approved SPDX license.
Authenticated repository access confirms control of the `EnderKeera/LayerC`
namespace, but the code-hosting URL must also be publicly reachable before a
Flathub submission. Flathub currently excludes general console software unless
an exception is granted, so this manifest is for local or self-hosted Flatpak
repositories until a publishing channel accepts it.

V2.6.1 grants only `--socket=x11`, which is required for the one Xlib
connection used by XI2, Shape, RandR, and EWMH. It does **not** grant network,
device, host-filesystem, Wayland, bus, IPC-share, or background access. X11
access is intentionally a separately reviewed permission because an X server
does not isolate clients; remove it to roll the Flatpak back to the V2.6.0
store/control-only mode.

```sh
flatpak-builder --user --install --force-clean build-flatpak \
    packaging/flatpak/io.github.EnderKeera.LayerC.json

flatpak run io.github.EnderKeera.LayerC                              # daemon (fg)
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

CI builds the manifest and runs daemon/control/X11 smoke under Xvfb. A physical
desktop is still required for real compositor, pointer, monitor, and GPU
observations; automated Flatpak success does not satisfy that checklist.

## CI and release boundary

The `linux` (`debian:stable`, `ubuntu:24.04`) and `linux-flatpak` jobs in
`ci.yml` run the gate above and are the automated Linux compatibility gate;
job details are in
[`../CONTRIBUTING.md`](../CONTRIBUTING.md#ci-and-release-gates). No Linux
package is published: `windows-msvc` produces the only release ZIP. Physical
input/click-through, real-monitor/driver behavior, and GNOME Xorg/KDE X11
native plus Flatpak evidence remain manual and pending until archived.
Future Gamescope or Wayland lanes could reuse the evidence schema, but no such
backend or claim is part of V2.6.
