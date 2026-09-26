# LayerC on Linux (V2.6)

This document owns Linux behavior, commands, packaging, permissions, and
physical evidence. Use [`../README.md`](../README.md) for the project-wide
release summary and current source/CMake/workflows for exact implementation and
gate details.

## Support status

Linux V2.6 is implemented in this tree but remains **pre-release**: package
metadata still reports 2.4.0 and the physical GNOME Xorg/KDE X11 evidence below
has not been archived. CI verifies headless, Xvfb, installed-native, and Flatpak
runtime behavior; it is not a substitute for the hardware checklist.

| Surface | State | Boundary |
| --- | --- | --- |
| V2.6.0 portable runtime | implemented | Foreground `layerd`, `layerctl`, XDG storage, Unix-socket control, bundles, install metadata |
| V2.6.1 X11 interaction | implemented | EWMH tracking, passive XI2 input, Shape overlays, RandR scaling |
| V2.6.2 capture/calibration | implemented | Composite/Damage capture, bounded fallback, `layercal` PPM/JSON output |
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
| Connection + extension probes | done | `layerctl status --json` publishes XI2/Shape/RandR/Composite/Damage/XFixes versions, EWMH availability, service state, capability availability, and stable reason tokens |
| Window tracking | done | EWMH `_NET_CLIENT_LIST` with a bounded root-child fallback; `GameMatch` resolution is name → path → class → title, case-folded |
| Input | done | XI2 raw key/button events plus hierarchy tracking for hotplug |
| Overlay | done | One click-through window per panel; Shape is required, otherwise the overlay stays hidden |
| Capture | done | Composite named pixmaps + Damage coalescing, with a 100 ms bounded direct-window fallback; exposed by `layercal` |

Input is **passive only**: events are read, never synthesized, and XTest is
linked into the test binary alone — never into `layerd`. Keys are normalized
from XKB keysyms (group 0, level 0 — the physical key, not the shifted
character) into the profile's Win32 virtual-key space, so `XK_e` becomes
`0x45`, matching a profile authored on Windows. Pointer buttons follow the
Windows daemon's numbering: X wheel buttons 4/5 map to profile codes 6/7 and
X side buttons 6/7 map to 4/5.

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
  title/class fallback when `/proc/<pid>/exe` is unreadable in Flatpak.
- Overlay visibility and click-through with a real pointer, including HiDPI
  panels and the missing-Shape hidden-overlay failure mode.
- Headless/non-X11 and missing-extension snapshots showing the affected
  adapter as unavailable rather than silently omitted.

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
LAYERC_BUILD_DIR=build-linux bash tools/run-linux-v26-evidence.sh gnome native 0x4600007
bash tools/run-linux-v26-evidence.sh gnome flatpak 0x4600007
LAYERC_BUILD_DIR=build-linux bash tools/run-linux-v26-evidence.sh kde native 0x4600007
bash tools/run-linux-v26-evidence.sh kde flatpak 0x4600007
```

The harness refuses non-X11 sessions, never synthesizes input, records the
status/capture/resource artifacts under `out/benchmarks/v26-linux/`, and
requires an explicit operator pass for the visibility, click-through, hotplug,
recovery, and visual-transition checks that cannot be proved automatically.
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
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure   # portable + Linux suites
```

Linux builds `layerd`, `layerctl`, and `layercal` when the required X11
libraries are present. The graphical editor, capture worker, and Game Bar host
remain Windows-only targets.

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
operator inspection rather than deleted. Name matching on both platforms uses
the shared deterministic fold (ASCII plus the cased Latin-1 supplement rows).
Import/export/reload commands work offline; after an import a running daemon
is notified when its socket exists. One daemon per socket — a live daemon
refuses a second bind.

Foreground loss or XI2 detachment clears transient held-key state while matched
sessions retain cooldowns and automation. When several windows match, a session
keeps its previous target in the background and prefers an active match on focus
gain. `layerctl stop`, SIGINT, and SIGTERM
run session-stopping automation and checkpoint persistent effects before the
sessions are destroyed; temporary capability quiescence only checkpoints.

## Install layout (GNUInstallDirs)

```sh
cmake --install build --prefix /usr/local        # BINDIR: layerd, layerctl, layercal
```

### Optional systemd user unit (default OFF)

```sh
cmake -S . -B build -DLAYERC_INSTALL_SYSTEMD_USER_UNIT=ON -DCMAKE_INSTALL_PREFIX=~/.local
cmake --install build
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
matching degrades to title/class and never broadens silently.

CI builds the manifest and runs daemon/control/X11 smoke under Xvfb. A physical
desktop is still required for real compositor, pointer, monitor, and GPU
observations; automated Flatpak success does not satisfy that checklist.

## CI and release boundary

The `.github/workflows/ci.yml` Linux matrix runs `tools/ci-linux.sh` in
digest-pinned `debian:stable` and `ubuntu:24.04` containers (the tags are display
labels). Headless and Xvfb suites are separate, required suite names are checked
against CTest registration and JUnit, and installed-native smoke is part of each
job. The repo is read-only; structured evidence is uploaded from a separate
writable mount. A dedicated Ubuntu job installs the Freedesktop 25.08 runtime
and SDK and runs the same gate with Flatpak coverage required.

These jobs are the automated Linux compatibility gate. `windows-msvc` remains
the package job and produces the Windows ZIP. Physical input/click-through,
real-monitor/driver behavior, and GNOME Xorg/KDE X11 native plus Flatpak
evidence remain manual and pending until separately archived.

The runner boundaries are intentionally extensible: future Gamescope and
Wayland lanes can reuse the evidence schema and runtime smoke contract, but no
such backend or compatibility claim is part of V2.6.
