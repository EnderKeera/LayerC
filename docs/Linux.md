# LayerC on Linux (V2.6 support; V2.7 in development)

This document owns Linux behavior, commands, packaging, permissions, and
physical evidence. Use [`../README.md`](../README.md) for the project-wide
release summary and current source/CMake/workflows for exact implementation and
gate details.

## Support status

The daemon and CLIs shipped in the `v2.6.0` release as **source/CLI only**.
The v2.6c feature set adds the X11 `layere` editor, a native Debian package,
and an editor Flatpak. These are included in package version **2.7.0**.
The version-tag workflow attaches the `.deb` and Flatpak bundle that its own
container and Flatpak gates built, installed and smoke tested (automated
evidence only); see
[Release packages](#release-packages). The [2026-10-21 source-bound local qualification](LinuxEditorPlan.md#rendered-audit-repairs-and-current-local-qualification-2026-10-21) covers the repaired calibration layout and faithful explicit-black appearance, directly inspected rendered states, active-operation peaks, selected validator-derived boundary/exhaustion envelope, fresh empty and representative idle, 100-cycle drift and the original 32-case walkthrough, plus fresh Windows/Linux and native-package/Flatpak gates. It binds the complete source snapshot and the final documentation-only delta; the rejected earlier rendered verdict and older measurements remain historical, not current qualification. The original 32-case count omitted a required controlled-target group and did not close the full walkthrough contract. Its [fresh 36-case successor](LinuxEditorPlan.md#required-overlay-walkthrough-audit-repair-2026-10-21) passes mandatory overlay focus/resize, actual click-through and cleanup in the owned Xvfb/Openbox session; it does not establish physical-desktop coverage.
The physical GNOME Xorg/KDE X11 evidence below is also **pending**: it has not been archived. `ci.yml` is
configured to verify headless, Xvfb, installed-native, package and Flatpak
runtime behavior. Release tags require a green `ci.yml` run; CI is
not a substitute for the hardware checklist. The
[Linux editor resource targets](#linux-editor-resource-targets) are
implementation contracts, not release acceptance claims.

| Surface | State | Boundary |
| --- | --- | --- |
| V2.6.0 portable runtime | implemented | Foreground `layerd`, `layerctl`, XDG storage, Unix-socket control, bundles, install metadata |
| V2.6.1 X11 interaction | implemented | EWMH tracking, passive XI2 input, Shape overlays, RandR scaling |
| V2.6.2 capture/calibration | implemented | Composite/Damage capture, bounded fallback, `layercal` PPM/JSON output |
| v2.6c X11 editor | implemented with automated evidence | FLTK `layere`: profiles, matching, cooldowns, bindings, panels, import/export, accessibility, automation, visual calibration, autosave and live drafts, diagnostics, device capture, appearance, detected tasks and live placement; X11 limits under [X11 editor](#x11-editor-v26c); installed, `.deb` and Flatpak gates; included in package version `2.7.0` |
| Physical native/Flatpak evidence | pending | GNOME Xorg and KDE Plasma X11 runs must be archived |
| Wayland (V2.7 preview) | preview implementation, automated evidence only | Native/degraded providers and evidence gaps below; no expanded desktop support claim |
| Gamescope | unsupported | Out of scope |

On X11, `layerd` reads passive input and paints click-through overlays; the
shared capture adapter also backs bounded CLI snapshots. Without `$DISPLAY`,
the daemon remains a headless store/control bridge and publishes graphical
capabilities as unavailable; `layercal` fails visibly. Name matching on both
platforms uses the shared deterministic fold (ASCII plus the cased Latin-1
supplement rows; all other codepoints compare byte-exact), replacing the
former Windows `LCMapStringEx` invariant mapping so Windows and Linux agree.

The package version is **2.7.0**. Typed overlay items and the Wayland preview
are included; support remains X11/headless. Publication does not expand the
supported desktop matrix, and historical V2.6 evidence does not qualify
additional desktop/compositor combinations.

## Linux editor resource targets

**These are approved implementation targets, not release acceptance claims.** The
ceilings established during toolkit selection remain unchanged. If a target is
missed, consult the user rather than raising it. The dated local evidence in
[`LinuxEditorPlan.md`](LinuxEditorPlan.md#rendered-audit-repairs-and-current-local-qualification-2026-10-21)
is not proof for every combination of valid profile data or physical-desktop
qualification.

Use Linux USS (private resident pages) as the primary editor memory metric, and
report PSS, RSS, swap, and any GPU/buffer allocations alongside it. Include every
editor-owned helper process in aggregate results. Record Windows private bytes
and working set separately; private commit and Linux USS are not equivalent
measurements.

| Scenario | Approved Linux USS ceiling |
| --- | ---: |
| Settled idle editor, no profile selected | 20 MiB |
| Representative store: 20 profiles, 10 cooldowns each, two panels each; one selected | 32 MiB |
| Active advanced view on a representative automation/visual fixture | 48 MiB peak |
| After 100 repeated dialog/profile-switch cycles | Within 2 MiB of the warmed baseline, with no continuing upward trend |

Capture the full startup peak even if the idle gate begins after settling. Once
functionality is available, measure worst-case valid documents derived from the
current validators. Before release, establish their explicit peak bound and
resource-exhaustion behavior; the representative fixture is not proof for every
valid profile.

Additional approved targets:

- Idle CPU at most 0.2% of one logical CPU averaged over 60 seconds when no
  animation or operation is active.
- Closing the editor leaves no editor/helper processes, owned sockets, or active
  capture resources.
- Opening/closing the editor must not weaken existing daemon memory/resource
  gates. Reuse the daemon benchmark contracts and add standalone editor
  measurement rather than interpreting daemon deltas as editor memory.
- Cache/history limits preserve authored data. A resource-limited preview or
  operation reports its reason; it does not silently alter a valid profile.

Keep the desktop, driver, video recorder, and agent/model overhead in a separate
harness measurement. Application-only numbers must not conceal the total cost of
running the testing environment.

## Wayland development (V2.7)

The backend is included as a V2.7 preview, not physically qualified.
[`Wayland.md`](Wayland.md) owns its architecture and provider contracts.
The supported Linux desktop scope remains X11; native Wayland is experimental.
Gamescope remains unsupported.

`layerd` selects a connected Wayland display in `auto`, then X11, then headless.
Use `LAYERC_DISPLAY_BACKEND=auto|x11|wayland` to force the choice; a forced
backend never silently selects the other session. `layerctl status --json`
reports `display.session`, compositor, and each provider's name, state and
reason tokens; the editor's Runtime Diagnostics repeats those rows.

- wlroots-family providers use Wayland outputs and layer-shell overlays.
  Foreign toplevels have no PID and only output geometry; with XWayland,
  tracking instead uses X11 and is explicitly degraded. KWin uses a loaded
  script and GNOME uses the optional Shell extension. These are implementation
  paths, not a compatibility promise for every compositor in those families.
- Input defaults to XWayland XI2 (`degraded`, `xwayland_focus_only`). The only
  global native input source is passive evdev, enabled with `layerd --input=evdev`
  or `LAYERC_INPUT=evdev`. The user must already belong to `input`; LayerC never
  elevates or changes groups. Devices are opened read-only and never grabbed.
- Native visual capture uses the ScreenCast portal and a portal-granted
  PipeWire connection. The user chooses a source; denial remains visible and
  does not trigger repeated prompts. A granted window stream is reported
  degraded `user_selected_window`, because the portal does not verify that the
  stream is the tracked window; `monitor_crop` marks the explicit degraded
  monitor mode. XWayland windows can retain X11 capture.
- `layere` remains an X11 client and requires XWayland. Its Detected Tasks,
  placement and calibration address XWayland windows only, not native windows;
  status and diagnostics do not hide that degradation.
- The optional GNOME extension installs under
  `<datadir>/gnome-shell/extensions/layerc@enderkeera.github.io/` but is never
  enabled by installation. For a native user install, use
  `cmake --install build-linux --component GnomeShellExtension --prefix ~/.local`
  and explicitly enable it with
  `gnome-extensions enable layerc@enderkeera.github.io`. Files inside a Flatpak
  cannot install or enable an extension in the host Shell.

Automated evidence is limited to headless sway (including scale 1/2 outputs),
private D-Bus fakes and GJS Shell/Mutter objects, synthetic evdev, and an
XWayland editor walkthrough. No real KWin, GNOME Shell, desktop portal consent,
GPU/physical monitor, real game, input hotplug or screen-reader user study was
observed. See [the compositor evidence matrix](Wayland.md#compositor-evidence-and-gaps).

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
V2.7 adds ordered cooldown, timer, counter, and indicator items. Counter pips
are bounded at eight; inactive indicator phases preserve row geometry. Missing
or mistyped sources remain visibly unavailable. Visibility, labels, values,
colors, and flash phases come from shared core presentation, including
item-scoped automation overrides, so the native and companion renderers do
not independently evaluate automation.
While an overlay is mapped, static panels add no repaint timer. Running
cooldowns and countdowns request a repaint at most 100 ms later, and flashing
indicators request their next half-period edge from shared presentation.
Expose, geometry, input and automation events still invalidate the overlay;
with no pending visual change, the event loop blocks even while a static panel
remains mapped.

The monitor scale comes from RandR physical size (clamped to 72–288 dpi, 96
when a server reports nothing usable), matching the Windows `MulDiv`-style
scaling so one profile looks the same on both platforms.

The Linux editor's Panels page uses an ordered item list with add, edit,
reorder, and remove. It exposes cooldown active duration, automation-timer
presets, numeric/pip counters, and indicators with flashing off or 0.5–3 Hz
(stored rates above 3 up to 4 Hz remain in the profile and flash at 3 Hz).
Preview and simulation use the same typed presentation. Existing legacy
cooldown-only profiles retain their `cooldowns` representation; schema 5 is
used for new typed features. See the [profile/item format](../README.md#overlay-items-v27-in-development).
The editor remains an X11/XWayland client; these controls do not add native
Wayland editor support or physical-desktop evidence.

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

### Physical qualification handoff

The matrix is pending for every physical desktop. Use a dedicated, disposable,
physical logged-in desktop account. Obtain explicit permission before
installations or session, display, or extension changes. Preserve the real
`DISPLAY`, `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, and session bus; do not redirect
`XDG_RUNTIME_DIR` away from compositor or portal sockets. Do not run native and
Flatpak sessions simultaneously or stop someone else's daemon.

| Physical session | Delivery | Status | Required evidence |
| --- | --- | --- | --- |
| GNOME Xorg | Native | pending — no physical host in this run | Identity/environment, interaction, capture, cleanup and resources below |
| GNOME Xorg | Flatpak | pending — no physical host in this run | Same, including sandbox matching and effective permissions |
| KDE Plasma X11 | Native | pending — no physical host in this run | Same |
| KDE Plasma X11 | Flatpak | pending — no physical host in this run | Same, including sandbox matching and effective permissions |

Start an evidence directory and identify the exact source state before each
physical run. This preflight is for a future physical tester; it was not run in
this handoff:

```sh
mkdir -p out/linux-physical
run=$(mktemp -d "$PWD/out/linux-physical/x11-XXXXXXXX")
git rev-parse HEAD > "$run/commit.txt"
git status --porcelain=v1 --untracked-files=all > "$run/worktree.txt"
git diff --binary HEAD > "$run/tracked-source.patch"
printf 'session=%s desktop=%s\n' "${XDG_SESSION_TYPE-}" "${XDG_CURRENT_DESKTOP-}" > "$run/session.txt"
uname -a > "$run/kernel.txt"
```

Record checksums for each changed tracked source file and each untracked source
file separately; the tracked patch does not capture untracked files. Record the
executable/package SHA-256, distro, kernel, session, compositor and GPU driver,
physical monitor scales/layout, backend status before/after, screenshots/video,
test PIDs, logs, and cleanup. Do not dump the full environment or include
secrets. For Flatpak, also capture the exact commit, package information,
effective permissions, and bundle hash; retain normal sandbox restrictions and
do not broaden device, host, or network access:

```sh
flatpak info --show-commit io.github.EnderKeera.LayerC > "$run/flatpak-commit.txt"
flatpak info io.github.EnderKeera.LayerC > "$run/flatpak-info.txt"
flatpak info --show-permissions io.github.EnderKeera.LayerC > "$run/flatpak-permissions.txt"
sha256sum /path/to/layerc-2.7.0-x86_64.flatpak > "$run/flatpak-bundle.sha256"
```

For native X11, launch only an installed build whose binary hashes were
recorded. Wait until the owned daemon is ready before the first status query; a
failed launch or status query is evidence, not a pass. Use only a disposable
profile store in the logged-in test account, and stop only the recorded
`test_pid`:

```sh
LAYERC_DISPLAY_BACKEND=x11 layerd > "$run/native-layerd.log" 2>&1 &
test_pid=$!
layerctl status --json > "$run/native-status-before.json"
# Activate a disposable profile in layere; exercise the checklist; capture status after.
layerctl status --json > "$run/native-status-after.json"
kill -TERM "$test_pid"
wait "$test_pid"
```

For Flatpak, use the same installed package and its documented commands, but
only its disposable private profile store in that account. Wait for the owned
daemon before the first status query, keep the normal sandbox, and stop only
this run's `test_pid`:

```sh
flatpak run --env=LAYERC_DISPLAY_BACKEND=x11 --command=layerd io.github.EnderKeera.LayerC > "$run/flatpak-layerd.log" 2>&1 &
test_pid=$!
flatpak run --command=layerctl io.github.EnderKeera.LayerC status --json > "$run/flatpak-status-before.json"
# Activate a disposable profile in layere; exercise the checklist; capture status after.
flatpak run --command=layerctl io.github.EnderKeera.LayerC status --json > "$run/flatpak-status-after.json"
kill -TERM "$test_pid"
wait "$test_pid"
```

Qualification includes passive E (`scan 0x12`) and mouse delivery to an
underlying controlled application, hotplug, executable/title/class matching
(including Flatpak's unreadable `/proc/<pid>/exe` case), and two independent
panels including ready-hide. Verify real pointer click-through and target
focus/move/resize, physical mixed DPI, Composite/Damage/fallback capture and
calibration, recovery/status faults, and that user-initiated close clears only
owned windows, processes, socket, and capture. Archive USS/PSS/RSS/swap/GPU
metrics and separate harness overhead against the existing resource target
table. An Xvfb walkthrough is not physical evidence. For every case record the
expected and observed result, artifact, and pass/fail/blocked; pending is not
passed.

## X11 editor (v2.6c)

The v2.6c editor is included in package version 2.7.0; publication does not
qualify physical desktops. Compared with the Windows editor, X11
has no Generic HID capture, Game Bar companion, capture exclusion or isolated
recognition (their data is preserved), the isolated-runtime setting has no
Linux counterpart (its value is preserved), and there is no Pick Window or direct
overlay drag because X11 overlays never take input. When X11 development libraries, FLTK 1.4+ with
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
panels. Cooldowns > Cooldown groups edits propagation groups and shared pools,
including members, timing, charges, recovery and retrigger, as one undoable
transaction. It has undo/redo, menus and context menus, and JSON/`.layercprofile`
import with collision review. Pack export has individual profile checkboxes,
Select all and Select none; an empty selection is refused and Cancel writes
nothing. Its preview is illustrative, not a complete renderer. The Automation tab edits rules and presets, runs the
deterministic simulator and records an authenticated live trace. The Visual
tab assigns, updates, clones and removes the selected cooldown's visual
trigger and calibrates it against an X11 window in a modal dialog: after an
explicit capture notice it previews the window through the bounded X11
capture, selects a region, samples a color, and edits average-color,
brightness, color-coverage and template predicates, with up to four captured
or imported PNG references and an editable alpha mask. Apply remains unavailable
until the current predicate has been evaluated against a captured frame. A
non-match is still an evaluation; changing the predicate or capture quality
requires reevaluation. Loading a template reference alone does not satisfy this
requirement. The profile, its sidecars and template assets are published in one
durable transaction.
Game Bar companion data and Generic HID bindings are preserved but not
editable. Unknown base-profile JSON fields are retained with their owning
objects, including nested fields and numeric source digits; unsupported or
invalid data is not dropped to make a save succeed. Unreadable/invalid module
data blocks Save rather than being silently discarded. Save uses the shared durable
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
Edits to a stored profile autosave: text fields after a 400 ms pause, other
edits immediately, through the same durable transaction, keeping focus and
undo/redo. A new profile needs one explicit Save; a legacy-format profile
pauses autosave until an explicit Save confirms the upgrade. Invalid field
text is never written. While an edit is pending (and no upgrade confirmation
is outstanding), the editor publishes the
accepted base profile to the daemon as an in-memory live draft (a typed,
versioned `draft` request on the authenticated control socket). The draft is
held by one editor under a 2.5 s monotonic lease that the editor renews; ending
it, or letting the lease lapse, restores the committed profile. Drafts never
reach the store, and module sidecars change only through the durable save.
View > Runtime diagnostics shows the daemon's process, X11, capability,
service, session and automation-fault state, refreshed on request, with the raw
JSON available for copying. Help lists the keyboard shortcuts (F1) and the
version. Captured bindings come from focused input only: keys, mouse buttons
and buttons of XI2 gamepad-class devices; generic HID capture is unavailable.
File > Save as writes the open profile and its modules under a new name, and
File > Open profiles folder starts `xdg-open` (or names the path when it is
missing). The editor follows the [v2 design system](DesignSystem.md): flat,
square controls, 1 px outlines, a 2 px accent focus ring without a focus tint,
and a 3 px accent bar on the selected tab. It uses the desktop interface font:
the XSETTINGS `Gtk/FontName` family when a settings manager runs, else
fontconfig's sans. Sizes from 11 to 14 px set the text directly; smaller sizes
stay at 11 px. Above 14 px, app-local FLTK screen scaling grows the authored
layout with the text. The LayerD status pane uses the body size. Tools >
Appearance applies the Windows presets (Graphite / Copper by default; System
keeps FLTK's palette), an accent and per-role colors, stored in
`<config>/layerc/editor-appearance.json`; Tools >
Options states that capture exclusion and isolated recognition are unavailable
on X11. Tools > Detected tasks (Ctrl+T) lists top-level EWMH windows (never the
editor) through a short-lived X11 connection, scanned only on open and Refresh,
and can set the profile's executable matcher. Position on window maps the
preview to the matching game window with the overlay's centred-anchor rule;
each drag is published through the live draft, so the overlay follows. Direct
Drag is not offered because X11 overlays never accept input.
Closing or selecting another profile flushes autosave first. It prompts to
save, discard or cancel only when edits remain unsaved: invalid field text, a
new profile, a failed save, or a declined upgrade. LayerD capabilities/faults are
visible, with an explicit refresh button. Status panes pair drawn status glyphs
with text; accessible names, descriptions and explicit save/reload announcements
remain available through AT-SPI. Help > Keyboard shortcuts uses measured key
and action columns, including F1, from the command registry.

The installed `layere` is smoke-tested by `tools/linux-editor-smoke.sh`: it
starts beside the installed daemon on a disposable display with private XDG
roots, must map its window, stay within the 20 MiB idle USS and 0.2% idle
CPU targets, and quit through Ctrl+Q. Xvfb walkthrough evidence is automated evidence, not physical
GNOME/KDE coverage or a screen-reader user study.
Gamescope and native Wayland remain unsupported in V2.6.

### GUI walkthrough runner and agent bridge

`tools/linux-editor-gui-tests.sh` runs the editor in a disposable session: a
private authenticated Xvfb display (1600x1000), Openbox, a private D-Bus
session, private HOME/XDG roots, the X11 smoke fixture as the game, `layerd`
and `layere`. It never addresses the user's display, bus, daemon, control
socket or profiles. Its clicks and keys are synthesized with `xdotool` only
inside that private display, as test input; `layerd` and `layere` never
synthesize input, and `tools/linux-gui` is not installed by CMake, the `.deb`
or the Flatpak. Every helper is owned, stopped and joined; a missing
dependency or build output fails the run rather than skipping it. The Python
AT-SPI checks also need `python3-gi`, `python3-pyatspi` and the matching
`gir1.2-atspi-2.0` metadata from the same AT-SPI installation (2.62 or later).
When using an extracted dependency root, point both `LD_LIBRARY_PATH` and
`GI_TYPELIB_PATH` at that root: new libraries paired with old system
introspection metadata can crash the test client. Dependency setup remains
separate from the ordinary run.

```sh
tools/linux-editor-gui-tests.sh --build build-linux --out out/linux-gui/manual
```

The default `walkthrough` mode drives `tools/linux-gui/walkthrough.py`:
bridge guard rejections; startup; the AT-SPI semantic check; keyboard
navigation; menus and context menus; profile create/rename/enable/delete with
undo/redo; invalid input left unwritten; edit, durable save, acknowledged
reload and a moved live overlay; import/export; automation JSON, simulator and
live trace; pixel and template calibration, shared-observation preservation
and the visual lifecycle; layout (resize, minimum size, maximize, scrolling,
text scaling); daemon stop, offline save and reconnect; and a visible store
failure that leaves the original profile intact. Advanced views are each
sampled for 60 s against the 48 MiB / 0.2% targets. A failed case blocks the
later cases; it never passes them.

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

`tools/submit-desktop-evidence.sh` wraps this harness and posts the run to a
pull request; see [DesktopEvidence.md](DesktopEvidence.md). It runs the harness
with `LAYERC_RUN_AUTOMATION=1 LAYERC_REQUIRE_AUTOMATION=1` and, in Flatpak mode,
first builds and installs the Flatpak from the checkout with
`flatpak-builder --user --install`. A posted report is advisory and does not
by itself change the pending status above.

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
- runs the six required X11 display, tracking, input, overlay, capture, and
  visual suites under Xvfb, failing if one is absent or skipped;
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
- with `--wayland off|auto|required` (default `auto`), runs the V2.7 `wayland`,
  `dbus` and `gjs` gates. Each suite owns its headless compositor or private bus;
  the caller's display, bus and input opt-in are cleared. `auto` records a
  missing lane/harness or an allowed skip as `unavailable`, never a pass.
  `required` makes every backend policy suite (including optional PipeWire)
  and the Wayland protocol/connection fixtures in the headless/X11 gates
  mandatory, and rejects their skips; unrelated PowerShell tooling stays
  optional. `off` records `not-run` and keeps source-only builds
  usable. Disposable container jobs install `tools/linux-wayland-deps.txt`
  unless this policy is off;
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
That job passes `--flatpak-only` (valid only with `--flatpak required`,
`--package off`, no `--wayland required` and no release evidence): it builds just the smoke fixture natively, records
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

In LayerC 2.7, `layerctl status` also prints a `display:` line with
session, compositor, and provider names, states, and fault reasons. JSON status
keeps the existing `x11` object and adds a separate `display` object. Backend
overrides, the opt-in evdev input source (`layerd --input=evdev` or
`LAYERC_INPUT=evdev`), and the in-development Wayland seam are described in
[`Wayland.md`](Wayland.md#seam); detection is not a native Wayland support claim.

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

### Release packages

Each `v*.*.*` release attaches `layerc_<version>-1_amd64.deb`,
`layerc-<version>-x86_64.flatpak` and `SHA256SUMS`. Verify the download, then
install the `.deb` as described under [Debian package](#debian-package), or
the Flatpak bundle, which fetches its runtime from Flathub:

```sh
sha256sum --check --ignore-missing SHA256SUMS
flatpak install --user ./layerc-2.7.0-x86_64.flatpak
flatpak run io.github.EnderKeera.LayerC          # layere
```

The bundle is unsigned (it is not served from a GPG-signed repository), so
`SHA256SUMS` from the release page is its integrity check.

### Debian package

```sh
cmake -S . -B build-deb -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr -DLAYERC_REQUIRE_EDITOR=ON
cmake --build build-deb --parallel
cpack --config build-deb/CPackConfig.cmake -B build-deb/cpack   # layerc_2.7.0-1_amd64.deb
```

CPack derives the library dependencies with `dpkg-shlibdeps` (needs
`dpkg-dev` and `file`) and adds the ATK, ATK bridge and AT-SPI ≥ 2.62 floor.
Debian 13's main archive has 2.56, so enable `trixie-backports` and install
from it explicitly:

```sh
sudo apt install -t trixie-backports ./layerc_2.7.0-1_amd64.deb
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

The manifest grants `--socket=x11` for XI2, Shape, RandR, EWMH and the
editor, plus `--socket=wayland` for the in-development V2.7 daemon providers.
It retains `--filesystem=xdg-documents` and `--filesystem=xdg-download` for
import/export. Its filtered session bus permits only the named compositor
services `org.kde.KWin`, `org.gnome.Shell` and
`io.github.EnderKeera.LayerC.GnomeShell` in addition to Flatpak's implicit
portal/accessibility access and app-ID name ownership. ScreenCast returns the
PipeWire fd; no broad PipeWire socket or session-bus grant is needed.

It grants **no** network, device, home/host filesystem, unfiltered session bus,
IPC-share or background access. evdev stays unavailable in the default sandbox;
only a user's explicit
`flatpak override --user --device=all io.github.EnderKeera.LayerC` can expose
input devices (the user's host permissions and LayerC's opt-in still apply).
LayerC never performs this override. X11 access is intentionally separately
reviewed because an X server does not isolate clients. Removing X11 disables
the editor (still X11-only); removing both display sockets returns to headless
store/control operation via `--command=`. Remove the two folders to restrict
file exchange to shell redirection. These permissions do not assert Wayland
support or install the GNOME extension in the host Shell.

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
headless build without the editor. `release.yml` reruns the
`debian:stable` gate and the Flatpak gate on the tag and publishes their
`.deb` and bundle with the Windows ZIP; `ci.yml` and `rolling` keep them as
gate evidence only. Physical
input/click-through, real-monitor/driver behavior, and GNOME Xorg/KDE X11
native plus Flatpak evidence remain manual and pending until archived.
Future Gamescope or Wayland lanes could reuse the evidence schema, but no such
backend or claim is part of V2.6.
