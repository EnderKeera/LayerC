# LayerC

[![CI](https://github.com/EnderKeera/LayerC/actions/workflows/ci.yml/badge.svg)](https://github.com/EnderKeera/LayerC/actions/workflows/ci.yml) ![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg) ![Windows](https://img.shields.io/badge/Windows-10_22H2%2B_%7C_11-blue.svg) ![Linux](https://img.shields.io/badge/Linux-X11_%2F_headless-lightgrey.svg)

LayerC is a lightweight, open-source overlay with a dependency-free C++20 core
and native platform backends. It provides passive input tracking, click-through
panels, opt-in visual recognition, bounded automation, typed JSON profiles, and
deterministic `.layercprofile` bundles. LayerC 2.7 adds cooldown,
timer, counter, and indicator items instead of limiting panels to cooldowns.

Windows is the complete desktop experience. Linux's supported desktop scope is
X11 with a headless fallback; the V2.7 Wayland backend remains a preview without
a support claim. Linux physical desktop evidence is still pending. LayerC has
no telemetry and never injects input, reads game memory, or requires elevation.

## Support and release status

The version in `CMakeLists.txt` is the package/release version. Milestone labels
below describe implementation scope; they do not make an unreleased milestone a
published release.

| Surface | State in this tree | Release boundary |
| --- | --- | --- |
| Windows desktop (`layerd`, `layere`, `layercal`, `layerw`) | Implemented, including typed overlay items | Windows CI, benchmarks, install, and ZIP packaging are the release path |
| Xbox Game Bar companion (V2.4) | Host, App Service, widgets, transport, and persistence implemented | Manual matrix, certification, and production signing remain; see [`docs/GameBarCompanion.audit.md`](docs/GameBarCompanion.audit.md) |
| Linux portable runtime (V2.6.0) | Headless daemon, control CLI, XDG store, bundles, and packaging implemented | Container CI covers automated behavior |
| Linux X11 input/overlay/capture (V2.6.1–V2.6.2) | Implemented with XI2, Shape, RandR, Composite, Damage, and XFixes | Physical GNOME Xorg/KDE X11 native and Flatpak evidence is pending; see [`docs/Linux.md`](docs/Linux.md) |
| Linux editor (v2.6c feature set; package version `2.7.2`) | FLTK X11 `layere` with the first-release feature set, AT-SPI accessibility, automation editing, visual calibration, autosave and leased live drafts, runtime diagnostics, device capture, appearance, detected tasks and live placement; X11 limits in [`docs/Linux.md`](docs/Linux.md#x11-editor-v26c) | Native `.deb` and Flatpak installed-editor gates and source-bound local rendered/resource/regression qualification pass; [36/36 required walkthrough cases](docs/LinuxEditorPlan.md#required-overlay-walkthrough-audit-repair-2026-10-21) include controlled-target focus/resize, actual click-through and cleanup; [evidence and scope](docs/LinuxEditorPlan.md#rendered-audit-repairs-and-current-local-qualification-2026-10-21); the release workflow publishes the gate-built `.deb` and Flatpak; physical evidence pending |
| Typed overlay items (V2.7) | Cooldown, timer, counter, and indicator items in the shared core, both editors, Win32/X11 overlays, Wayland/GNOME frames, and Game Bar slots | Shared presentation in release packages; no expanded desktop/hardware support claim |
| Wayland (V2.7 preview) | Native providers, explicit XWayland degradation, opt-in passive evdev, portal capture and GNOME extension implemented as a preview; the editor remains an XWayland client | Automated headless sway and private-bus fixtures only; no support claim or physical KWin/GNOME evidence; see [`docs/Wayland.md`](docs/Wayland.md) |
| Gamescope | Not supported | Out of scope |

Package metadata reports **2.7.2**: the typed overlay items and Wayland preview
of 2.7.0, the accessible editor design system and signed launcher updates of
2.7.1, and the Windows editor's launcher update setting. The version-tag
workflow publishes packages only after its Windows, Debian, and Flatpak gates
pass. Publication does not expand the supported desktop matrix; dated evidence
retains its stated scope.
Windows ZIP packaging starts LayerC via `layerd.exe` and also contains the
`layerc.exe` launcher, whose self-updates remain OFF by default
([details](#windows-launcher-and-updates-v26b)). Physical Linux evidence and the
Game Bar companion's manual matrix, certification, and production signing
remain open.

## Non-negotiable guarantees

- **Passive input only:** LayerC reads input events; it never blocks or
  synthesizes them. XTest is linked only into a Linux test binary. The only
  exceptions are two Windows editor modes that you start yourself. **Pick
  window** captures the mouse until your next left click picks the window
  under it; Esc or a capture loss cancels it. **Direct drag** asks for consent
  first. Then, during Position on window, LayerD's visible panels take the
  clicks that land on them until you turn it off or the edit ends. Neither mode
  installs hooks or synthesizes input.
- **No anti-cheat bypass:** no injection, hooks, game-memory reads, drivers, or
  administrator elevation.
- **No telemetry or network:** exceptions are the optional same-package Game
  Bar App Service `LayerC.GameBar.v1` and the Windows `layerc.exe` launcher.
  Only the launcher may make anonymous HTTPS GETs to this repository's GitHub
  release at launch, with user-disableable updates, then start `layerd` and exit.
  `layerd`, `layere`, `layercal`, and `layerw` never access the network or link
  an HTTP client.
- **Monotonic timing:** cooldowns and timers use monotonic clocks.
- **Bounded work:** visual sampling and automation have explicit resource and
  execution caps.

## Quickstart

### Windows

Requirements: Windows 10 22H2+ or Windows 11 x64, Visual Studio 2026 with the
Desktop C++ workload, and CMake 3.28+. The optional Game Bar package also needs
Universal Windows Platform and C++ UWP tools; see [local MSIX signing and
sideloading](packaging/windows/GAMEBAR-SIDELOAD.md) for development setup.

```powershell
# Focused local iteration
cmake --preset vs2026
cmake --build --preset vs2026-debug
ctest --preset vs2026-debug

# Full local Release package: build, test, install, and ZIP
cmake --workflow --preset vs2026-release
```

Installed binaries land in `out/layerc/`.

```powershell
# Run the daemon with disposable example profiles. LAYERC_SETTINGS_KEY likewise
# moves settings from HKCU\Software\LayerC to another HKCU-relative key.
$env:LAYERC_PROFILE_DIR = "$PWD\examples"
$env:LAYERC_SETTINGS_KEY = 'Software\LayerC.Dev'
.\build\vs2026\Debug\layerd.exe

# In another terminal: Shift+E triggers the example Dash cooldown
.\build\vs2026\Debug\lctest.exe
```

### Linux

Requirements: a C++20 compiler, CMake 3.28+, and Ninja or Make. X11 development
libraries enable input, overlays, capture, and `layercal`; without them LayerC
builds a headless store/control daemon. FLTK 1.4+ with `fltk-config` (or,
where the distribution lacks it, the pinned FLTK 1.4.5 in `third_party/fltk`)
and sd-bus development files additionally enable the Linux `layere` editor; see
[`docs/Linux.md`](docs/Linux.md#x11-editor-v26c).

```sh
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure

./build-linux/layerd                 # foreground daemon
./build-linux/layerctl status --json # control/status CLI
```

Debian/Ubuntu dependency names, Xvfb tests, installation, systemd, Flatpak,
calibration, and hardware evidence are documented in [`docs/Linux.md`](docs/Linux.md).

## Programs

| Program | Windows | Linux | Purpose |
| --- | ---: | ---: | --- |
| `layerc` | yes | no | Per-user launcher with opt-in signed updates; starts the installed daemon and exits (v2.6b; in the Windows ZIP since 2.7.1) |
| `layerd` | yes | yes | Overlay/automation daemon; background desktop app on Windows, foreground service on Linux |
| `layere` | yes | X11 (v2.6c) | Full native editor on Windows; on Linux the X11 editor (v2.6c feature set, included since `v2.7.0`; limits in [`docs/Linux.md`](docs/Linux.md#x11-editor-v26c)) |
| `layercal` | GUI | CLI on X11 | Visual calibration helper or bounded PPM/JSON snapshot tool |
| `layerw` | yes | no | Demand-launched isolated WGC capture worker with a 256 MiB Job limit |
| `layerctl` | no | yes | Status, reload, stop, and transactional profile/bundle import/export |
| `layerc-gamebar` | optional MSIX | no | Xbox Game Bar widget host and same-package App Service |

## Windows launcher and updates (v2.6b)

The portable Windows ZIP starts LayerC via `layerd.exe`. It also contains
`layerc.exe` when `LAYERC_UPDATE_PUBLIC_KEY` is non-empty, which by default it
is: CMake reads the committed release key from
`packaging/windows/update-signing-key.hex`. The launcher and its tests are built
either way.

`layerc.exe` installs its launcher per user under
`%LOCALAPPDATA%\Programs\LayerC`, registers Apps & Features and a Start menu
shortcut, starts the already-installed `layerd`, and exits. Autostart is a
separate opt-in (`--autostart=on|off`).

**Self-updates are OFF by default.** An unset, unreadable, or non-DWORD update
preference stays OFF.
With updates OFF, the launcher skips the update path entirely, performs no
network I/O, and still starts the installed daemon. Keep an existing install
or run the packaged `layerd.exe` directly; an offline launcher cannot download
a first build.

The preference is `HKCU\Software\LayerC\Updates` (REG_DWORD); a nonzero value
opts in, zero or deleting the value opts out. The Windows editor sets it from
Tools > Options > **Install updates when LayerC starts**, which reads an unset
or unreadable value as off. In PowerShell:

```powershell
# Opt in for subsequent launches
reg add HKCU\Software\LayerC /v Updates /t REG_DWORD /d 1 /f
# Opt out persistently (also the default when the value is absent)
reg add HKCU\Software\LayerC /v Updates /t REG_DWORD /d 0 /f
# Skip the update check for just this launch, regardless of the preference
& "$env:LOCALAPPDATA\Programs\LayerC\layerc.exe" --offline
```

Opting in never bypasses verification: without a valid embedded release key,
even an enabled launcher makes no update requests. Checks are also skipped
while `layerd` is running. Exit it before checking for an update or rolling
back with `--rollback`.

Activation is ordered after durable payload publication: all downloaded and
reused files, the installed manifest, and their directories cross the platform
durability barriers before `current` is durably replaced. A failed or unconfirmed
barrier stops activation and retains recovery evidence. For a new build ID, an
interruption before activation keeps the previous build selected. Same-build-ID
repairs park the old directory until the replacement is committed and recover
it on the next read if the replacement directory is absent.

An opted-in launcher follows tagged releases only. Each `v*.*.*` tag's
`release.yml` run signs the tested install tree with the ECDSA P-256 release
key and publishes `manifest.json`, its detached DER `manifest.sig`, and the
SHA-256-named file assets to the `rolling` release. The rolling `main` ZIP is
not an update. The feed's sequence follows the version, so a launcher never
moves to an older release. Until the first tag carrying the release key is
published, there is no feed, and an opted-in launcher reports the update as
skipped. Signing, key provisioning and the publication order are in
[CONTRIBUTING.md](CONTRIBUTING.md#release-signing-windows-updates). Offline
preparation and verification are in the
[Windows update signing instructions](packaging/windows/UPDATE-SIGNING.md).

## Build and package commands

### Windows presets

| Task | Command |
| --- | --- |
| Configure Visual Studio | `cmake --preset vs2026` |
| Build Debug | `cmake --build --preset vs2026-debug` |
| Test Debug | `ctest --preset vs2026-debug` |
| Build Release | `cmake --build --preset vs2026-release` |
| Test Release | `ctest --preset vs2026-release` |
| Install | `cmake --install build/vs2026 --config Release --prefix out/layerc` |
| Build, test, install, and ZIP | `cmake --workflow --preset vs2026-release` |
| MinGW cross-check | `cmake --workflow --preset mingw-ninja` (requires `MINGW_ROOT` and Ninja) |
| Game Bar MSIX | `cmake --build --preset vs2026-release --target layerc_gamebar_msix` |

### Linux install

```sh
cmake --install build-linux --prefix /usr/local
```

This installs `layerd`, `layerctl`, and, when X11 support was configured,
`layercal`; with the editor, also `layere`, its desktop entry, icons and
metainfo. The optional systemd user unit is default-off here; the Debian
package installs it without enabling it. The Debian package
(CPack) and Flatpak details live in [`docs/Linux.md`](docs/Linux.md).

## Capabilities

### Shared core

- Typed, versioned UTF-8 JSON profiles and sidecars.
- Ordered cooldown, timer, counter, and indicator panel items with shared
  presentation and item-scoped automation overrides; see [overlay items](#overlay-items-v27-in-development).
- Cooldown state machines with charges, ignore/restart retrigger policies, and
  optional standalone active-duration windows.
- Demand-driven `DaemonKernel`, `CapabilityDemand`, and runtime-service lifecycle.
- Deterministic miniz ZIP bundles with SHA-256 asset deduplication and
  crash-recoverable transactions. Unconfirmed durability retains recovery evidence;
  removed modules stay disabled through durable tombstones.
- Declarative Automation Graphs reacting to input, visual triggers, cooldowns,
  timers, and variables. Caps: 64 rules, 64 variables, 32 timers, 256 AST nodes,
  depth 16, and a cooperative 2 ms root-event budget. Exhaustion rejects the
  current event without disabling unrelated automations. The deadline is checked
  before external commit; synchronous callbacks are not forcibly preempted.

### Windows desktop

- Buffered Raw Input keyboard/mouse and XInput controller input.
- Non-activating, click-through Win32 GDI panels attached to matched game HWNDs.
- WGC capture with Desktop Duplication fallback; 64-pixel Low and 256-pixel
  Balanced sampling grids.
- Numeric visual predicates and PNG template matching with alpha masks.
  Template assets are stored as non-interlaced 8-bit RGBA PNG on both
  platforms; imported PNGs and bundle assets in any other standard PNG format
  are re-encoded on import, and their references follow the new content hash.
  Apply remains unavailable until the current predicate has been evaluated
  against a captured frame. A non-match is still an evaluation; changing the
  predicate or capture quality requires reevaluation. Loading a template
  reference alone does not satisfy this requirement.
- Native editor, live preview, simulator, diagnostics, resilient executable-name
  matching, and optional exact-path matching. Local live-edit protocol 5 and
  calibration protocol 2 carry profile documents as nested JSON objects (null
  for absent sidecars); mismatched peers report an error instead of applying
  edits. Restart the daemon and editor/calibrator together after an update.
- Bar preview preserves the target client area's aspect ratio and scales bar
  dimensions in device-independent pixels; without a live target it uses a
  1920 × 1080 reference. The simulator replays its exported events from empty
  persistent state, so it does not reproduce saved daemon state. Trace replay
  imports recorded roots only; derived events remain audit-only. Legacy traces
  without explicit root provenance must be recaptured.
- Optional isolated capture worker and Xbox Game Bar companion.

### Linux X11

- XDG-rooted transactional storage and current-user `AF_UNIX` control channel
  authenticated with `SO_PEERCRED`.
- Passive XI2 keyboard/mouse/gamepad-class input and EWMH window tracking.
  Focus loss clears held keys while preserving cooldowns; final shutdown runs
  session-stopping automation and checkpoints its persistent effects.
- Click-through Shape overlays with RandR monitor scaling and the same typed
  overlay items as Windows.
- Composite/Damage capture with a bounded direct-window fallback.
- `layerctl` profile/bundle transfer compatible with Windows JSON and
  `.layercprofile` payloads.
- `layere` editor (v2.6c) on X11; no tray, Wayland backend, or Gamescope
  support in V2.6.

### Xbox Game Bar companion

- Eight fixed panel slots and one settings widget, including all four typed
  item kinds through bounded immutable presentation snapshots.
- Same-package App Service `LayerC.GameBar.v1`.
- Immutable snapshots capped at 64 KiB, with stale data cleared after 2 seconds.
- Zero unpackaged runtime cost. See [`docs/GameBarCompanion.md`](docs/GameBarCompanion.md).

## Test targets

CMake registers only the targets available on the configured platform. X11
tests skip with code 77 when a display or required extension is unavailable.

### Cross-platform

| Target | Coverage |
| --- | --- |
| `layerc_core_tests` | Profiles, UTF-8, cooldowns, automation, live edit, visual-profile logic, and update manifests |
| `layerc_launcher_tests` | Launcher update engine: signed manifest checks, delta downloads, atomic build switch, repair, and rollback (v2.6b) |
| `layerc_runtime_tests` | Capability demand, lazy lifecycle, reconciliation, rollback, and stress |
| `layerc_capture_tests` | Capture math, normalization, and Low/Balanced/Exact bounds |
| `layerc_template_png_tests` | Shared template PNG codec (every color type, depth, and Adam7), deterministic canonical encoding, and bundle-import asset canonicalization |
| `layerc_automation_persistence_tests` | Shared per-profile automation persistence: debounced checkpoints, retried write faults, state-load repair, and values that outlive sessions |
| `layerc_automation_benchmark` | Automation evaluator budget (`<= 0.25 ms` average, `<= 2.0 ms` p99); runs serially in CTest; maximum is reported, not gated |
| `layerc_benchmark_result_tests` | Benchmark result-writer contract (registered when PowerShell is available) |
| `layerc_visual_asset_gc_contract_tests` | No automatic collection from production edit/calibration paths; recovered store ownership for offline maintenance |
| `layerc_editor_document_tests` | Shared editor document: cooldown-removal dependencies, undo/redo, the commit pipeline, store round trip, sidecar recovery, and upgrade consent |
| `layerc_editor_automation_tests` | Shared automation editor: presets and graph edits, JSON error locations, trace files and control messages, simulator replay, and panel item previews |
| `layerc_editor_diagnostics_view_tests` | Runtime diagnostics text: connection and snapshot states, foreground session summary, Linux provider details, visual worker states, and error precedence |
| `layerc_editor_visual_tests` | Visual assignment: calibration defaults and bounds, template cropping and masks, reference validation, shared observation updates and clones, invalid sidecar replacement, and observation limits |
| `layerc_editor_color_tests` | Editor color math: contrast ratios, hue and accent selection, and overlay contrast warning text |
| `layerc_editor_overlay_preview_tests` | Overlay preview painting (CTest label `editor`): Windows mirrors the daemon's GDI bar, card, charge and text painting; Linux takes geometry and paint operations from the shared display list |
| `layerc_editor_accessibility_model_tests` | Toolkit-neutral editor accessibility model: validated atomic tree publication, text geometry checks, and value-copy boundaries |
| `layerc_editor_accessibility_changes_tests` | Toolkit-neutral differences between two accessibility snapshots: additions, ordered structure, property and state changes, text/value spans, caret and selection, one focused node, window activation, announcements as occurrences, and silent geometry changes |
| `layerc_fltk_accessibility_tree_tests` | FLTK widgets published to the toolkit-neutral accessibility tree (CTest label `editor`; on Linux under an X display): consistent snapshots, expansion and scroll state, actions, button roles, and field text equivalence, work bound and snapshots |

### Windows

| Target | Coverage |
| --- | --- |
| `layerc_launcher_win_tests` | Windows launcher: CNG signature verification, default-OFF update preference and opt-in/out, per-user registration/autostart in disposable HKCU keys, and the Start menu shortcut (v2.6b) |
| `layerc_capture_integration_tests` | Interactive WGC/DD integration; registered only with `LAYERC_ENABLE_INTERACTIVE_CAPTURE_TESTS=ON` |
| `layerc_overlay_tests` | HWND attachment, GDI rendering, capture exclusion, `lctest` matching, and Direct drag: the overlay takes the pointer only while Direct drag is on, and ending it or plain edit mode leaves the pointer with the target window |
| `layerc_platform_tests` | Profile store, transaction recovery, named-pipe IPC, and peer authentication |
| `layerc_daemon_runtime_tests` | Daemon maintenance/input demand plus independently composed Windows runtime owners: input dispatch, idle/quiesced resources, automation lifecycle/trace, and live-control startup/fault recovery |
| `layerc_bundle_tests` | Deterministic bundle export/import and SHA-256 asset deduplication |
| `layerc_isolated_visual_tests` | `layerw` protocol, bounds, authentication, and quarantine behavior |
| `layerc_companion_tests` | Companion sidecars, snapshots, transport, and stale expiration |
| `layerc_editor_tests` | Editor dialogs, v2 contrast/focus/glyph and contrast-theme seams, layout and accessibility contracts, schema helpers, automation palette, and the desktop-identity decision for disabling IME in tests |
| `layerc_editor_live_edit_tests` | Editor live-edit session: package authentication, pending target expiry, reconnect and target loss, anchor revisions and throttling, heartbeats, and committed-profile publication |
| `layerc_calibration_tests` | Calibration window layout, behavior, and worker interaction |
| `layerc_windows_fltk_accessibility_tests` | FLTK editor (`layere_fltk.exe`, built but not shipped): a real `IUIAutomation` client reads a fixture window's names, roles, descriptions and reading order, edits and invokes through Value/Invoke, checks read-only refusal, text ranges, text/selection/value/focus events and status notifications, and sees providers disconnect on close |
| `layerc_windows_fltk_notification_tests` | FLTK editor: a real UI Automation client receives child-list and accessible-name changes, but not announcements made before it connected |
| `layerc_windows_fltk_patterns_tests` | FLTK editor: a real `IUIAutomation` client drives Toggle, RangeValue, Selection/SelectionItem, ExpandCollapse and Scroll, an idle client costs at most one tree walk per refresh interval, and app-side changes are published without a client request |
| `layerc_windows_fltk_text_tests` | FLTK editor: a real client drives Text and TextPattern2 on single-line, multiline and scrolled fields: line and endpoint movement, search, single-selection and child-range contracts, text and selection events, insertion-point geometry, offscreen range refusal, and Windows system-caret position and lifetime |
| `layerc_windows_fltk_binding_tests` | FLTK editor: focused keyboard and mouse events, background-dialog refusal, IME-message guards, XInput edge mapping, and HID dialog validation and undo with a fixture device |
| `layerc_windows_fltk_model_tests` | FLTK editor model, from the shared model cases against a test daemon: validated edits and durable save/roundtrip, selection identity across a rescan, atomically refused invalid candidates, visible write and invalid-module failures, appearance persistence and contrast floors (the shared refusal wording, every preset, role overrides and the System theme), detected tasks and placement, status and announcement contracts, and stable process identity; the POSIX folder launcher and headless X11 calibration cases do not run on Windows |
| `layerc_windows_fltk_session_tests` | Toolkit-free editor session shared with Linux: drafts and history, mutation candidates with preserved data, save/delete failures, enablement and invalid modules, reviewed import/export and import races, automation and visual candidates with history and recovery, atomic template duplication, calibration assets and stale tokens, field parsing, and change listeners |
| `layerc_windows_fltk_widget_tests` | FLTK editor widgets, from the shared widget, panel-item, dialog, visual-contract, diagnostics, review, command-registry and design-system paint cases: forms, dirty prompts, menus and shortcuts, layout, dialogs including Appearance's contrast refusal, offscreen painting, the profiles-folder command through a launcher seam, calibration target cancellation, and Pick window ending on Escape, a click, capture loss or main-window close with native capture released and the pointer back on a disposable target window; AT-SPI, X11 focus-capture and in-process X11 calibration cases are printed as not applicable with the Windows suite that covers each |
| `layerc_windows_fltk_autosave_tests` | FLTK autosave: debounce, undo and offline save, first save of a draft, invalid-buffer, write-failure and refused-save recovery, no automatic upgrade prompt, modal suspension and flush on close, a locked target file saved on retry, and interrupted-import recovery |
| `layerc_windows_fltk_ime_tests` | FLTK IME integration: IMM input contexts on the main window and dialogs, candidate-window placement at single-line and multiline carets, and `WM_IME_CHAR` text (CJK and a surrogate pair) reaching the field model and UI Automation; composition through an installed IME runs only when a Japanese, Chinese or Korean IMM layout is installed, otherwise the suite prints that it was not observed |
| `layerc_windows_fltk_settings_tests` | FLTK editor settings in a disposable settings key: the shared appearance record and legacy migration, Options actions and descriptions, capture exclusion on existing and new windows, text scale at startup, by shortcut and across DPI changes, contrast-theme mapping and desktop listener registration across multiple listeners (including recreation after the last closes), Appearance floors set aside under a contrast theme, title-bar decoration, tooltips and menu keys, and stepped fields with preview nudging |
| `layerc_windows_fltk_startup_tests` | FLTK editor startup: a second instance raises the running editor, the single-instance guard name shared with the Win32 editor for the logon session, the profile-recovery failure text, and the product line and window titles |
| `layerc_windows_fltk_calibration_tests` | FLTK visual calibration with a disposable store and settings key: the FLTK pump runs timeouts and restores the owner window, hiding the owner ends the helper without quitting the editor, an unavailable owner is refused before target resolution or launch, failure text matches the Win32 editor, and the platform service accepts and reports a result |
| `layerc_windows_fltk_position_tests` | FLTK Position on window against a fixture edit service: begin, toggle and preview drag with Direct Drag off by default, incoming anchors and accepted edits, end reasons and expiry, and ending the edit when the profile is switched or closed |
| `layerc_windows_fltk_on_window_tests` | On-window edit session over the daemon hub: authenticated begin and shutdown, the latest profile awaited before expiry or refusal, coalesced profiles with throttled anchors and Direct Drag, the target window closing during an edit, and peer events and discard |
| `layerc_windows_fltk_daemon_link_tests` | FLTK daemon hub against a fake `layerd` on a private pipe: the lazy worker and absent status, reloads that start `layerd` once without claiming success or wait for a committed acknowledgement, `layerd` error text, reply deadlines, refused clients without a launch, a busy pipe, one held connection for status, reload, trace and edit, trace subscribe, push, unsubscribe and retries, edit heartbeats and expiry, the exact image allowlist, and visual and automation retry replies |
| `layerc_windows_fltk_trace_relay_tests` | Automation trace relay: one reset per fresh subscription, in-order roots, overflow that keeps the newest roots and reports a gap, packet budgets, reconnect gaps, failure after three attempts, invalid packets, replaced subscriptions, and whether `layerd` may still hold an unsubscribed one |
| `layerc_windows_fltk_smoke` | `layere_fltk --smoke` with an isolated profile directory and settings key: takes the single-instance guard, recovers the store, opens the editor window and its UI Automation tree, then closes |

### Linux

| Target | Coverage |
| --- | --- |
| `layerc_linux_profile_store_tests` | XDG roots, permissions, journaling, fault injection, and recovery |
| `layerc_linux_visual_assets_tests` | Profile template assets: canonical PNG storage and import, retained references, unsafe destination refusal, atomic batch publication, writer contention, and interrupted-journal recovery |
| `layerc_linux_profile_pack_tests` | Byte-deterministic bundle export and import roundtrip |
| `layerc_linux_event_loop_tests` | Monotonic timerfd deadlines, signal shutdown, and zero idle wakeups |
| `layerc_linux_display_adapters_tests` | Display selection, strict overrides, compositor hints, headless/provider reasons, and no-graphics builds |
| `layerc_linux_display_types_tests` | Display geometry: DPI-scaled panel paint operations, byte-stable display list JSON, and monitor overlap and fallback |
| `layerc_linux_session_engine_tests` | Provider-neutral session engine with fixtures: admission, capability demand, input and overlay dispatch, resource release, and automation checkpoint retry after provider loss |
| `layerc_linux_display_route_tests` | Display route planning with fixture capabilities: provider priorities and fallbacks, degradation reasons, and stable provider names |
| `layerc_linux_display_route_binding_tests` | Display route execution with fake providers: session admission, matching input, overlay and capture dispatch, XI2 fallback after evdev failure, and rebinding as window reports change |
| `layerc_linux_display_x11_tests` | X11 orchestration/status compatibility and an idle blocking event loop (CTest label `x11`) |
| `layerc_linux_wayland_connection_tests` | Optional Wayland transport: private failed/timeout sockets, bounded startup, descriptor cleanup |
| `layerc_linux_wayland_registry_tests` | Optional test-only Wayland server: registry/removal, connection loss, and compositor selection |
| `layerc_linux_wayland_xwayland_loss_tests` | Optional Wayland server over Xvfb: degraded providers and forced Wayland loss without undrained XWayland work |
| `layerc_linux_wayland_keymap_tests` | Optional test-only Wayland server: seat keyboard keymap delivery, format/size bounds, capability loss, and seat removal |
| `layerc_linux_wayland_paint_tests` | Layer-shell painter without a compositor: display-list operations on exact device pixels at integer buffer scales |
| `layerc_linux_wayland_native_tests` | Wayland lane under headless sway (CTest label `wayland`): outputs, layer-shell panels, foreign toplevels, and the display seam end to end |
| `layerc_linux_evdev_input_tests` | Passive evdev source over FIFO-backed synthetic devices: classification, read-only opens, key/button/wheel/hat mapping, `SYN_DROPPED` resync, and hotplug (CTest label `evdev`) |
| `layerc_linux_evdev_keymap_tests`, `layerc_linux_evdev_us_keymap_tests` | evdev keycode translation with xkbcommon and with the built-in US table |
| `layerc_linux_evdev_daemon_tests` | Opted-in evdev route in the daemon: no opens without the opt-in, demand-driven open/close on the event loop, cooldown triggers, hotplug registrations, and permission/missing-class reasons |
| `layerc_linux_evdev_boundary` | Lexical check that evdev sources never grab, write, or open a device for writing |
| `layerc_linux_control_channel_tests` | Unix-socket framing, peer UID, stale sockets, and `layerctl` commands |
| `layerc_linux_control_ownership_tests` | Bind-failure cleanup, descriptor reuse, and preserved startup error (test-only linker fault simulation) |
| `layerc_linux_live_draft_tests` | Live profile drafts: request and reply validation, in-memory replacement, revision and owner checks, monotonic renewal and expiry, and control dispatch |
| `layerc_linux_lease_worker_tests` | Editor lease worker: submission wakeups, monotonic deadlines, stopping an idle worker and refusing further work, and bounded client identifiers |
| `layerc_linux_editor_daemon_link_tests` | Editor status/reload: offline vs inspection faults, queue deadlines, live-socket preservation, refused and lost acknowledgements |
| `layerc_linux_editor_atspi_mapping_tests` | Display-free (CTest label `editor`) AT-SPI mapping of the editor accessibility model: role codes and names, refined text-field roles, state bits and state-event names, exposed interfaces, UTF-8/character offset translation and reading-unit boundaries |
| `layerc_linux_atspi_adaptor_tests` | The editor's own sd-bus AT-SPI bridge on a private bus with fake `org.a11y.Bus`/registry services (CTest label `editor`): idle until a consumer listens, one embedded application, tree reads, protocol events and throttling, actions, values, selection, component geometry, character-offset text and edits through the field's own path, `NO_AT_BRIDGE`, and reported/recovered missing launcher, failed bus, refused embed and lost bus |
| `layerc_linux_editor_platform_tests` | Editor platform services: stable process identity, folder launcher arguments and exit status, an idle missing launcher, and a visible calibration-unavailable fault |
| `layerc_linux_editor_model_tests` | Production editor session: validated profile edits and durable save/roundtrip with retained cooldown/panel/binding fields, selection identity across a rescan, atomically refused invalid candidates, honest offline reload, and appearance (preset and override contrast floors with the shared refusal wording, status glyph classification, outlines, desktop text scaling and XSETTINGS parsing)/task/placement helpers |
| `layerc_linux_editor_session_tests` | Toolkit-free editor session: drafts and history, mutation candidates with preserved data, save/delete failures, enablement, reviewed import/export and import races, and field parsing |
| `layerc_linux_editor_widget_tests` | FLTK editor under an X display (CTest label `editor`): drafts and history, cooldown/binding/panel forms, dirty prompts, binding capture, menus, measured shortcuts, Appearance's contrast refusal, small-window layout, AT-SPI semantics, and offscreen focus/tab/glyph painting |
| `layerc_linux_editor_autosave_tests` | FLTK autosave under an X display (CTest label `editor`): debounced edits, undo and redo, offline save, first explicit save for new drafts, recovery from invalid fields and write failures, upgrade consent, modal suspension, and flush on close |
| `layerc_linux_x11_tests` | X11 probes, EWMH tracking, and game matching |
| `layerc_linux_x11_registry_tests` | Display-free session admission, automation health, checkpoint retry, and bounded status diagnostics |
| `layerc_linux_x11_input_tests` | XI2 normalization, hotplug, mouse/wheel mapping, and session replay |
| `layerc_linux_x11_overlay_tests` | Shape click-through, panel geometry, monitor scaling, and painted output |
| `layerc_linux_x11_capture_tests` | Composite/Damage capture, bounded fallback, resources, faults, and `layercal` output |
| `layerc_linux_x11_visual_tests` | Visual demand/session integration and transition behavior |

## CI and releases

CI runs on pushes to `main`, pull requests, and manual dispatch: workflow
lint, Windows MSVC build/test/benchmarks/package, MSVC `/analyze`, Linux
containers, and Flatpak smoke. Green `main` publishes the `rolling`
prerelease; `v*.*.*` tags publish a release. Package contents and their gates
are defined by the current release workflow, not by the local version bump.
Jobs, runners, artifacts, and hardware benchmarks are described in
[`CONTRIBUTING.md`](CONTRIBUTING.md#ci-and-release-gates).

## Profile locations and format

| Platform | Base profiles | Sidecars/state |
| --- | --- | --- |
| Windows | `%LOCALAPPDATA%\LayerC\profiles\<id>.json` | Subdirectories under the same profile root |
| Linux | `$XDG_CONFIG_HOME/layerc/profiles/<id>.json` (default `~/.config`) | `$XDG_DATA_HOME/layerc/profiles/<id>/` (default `~/.local/share`) |

Use `layerctl profile` or `layerctl bundle` commands to transfer data between
Windows and Linux; do not copy an entire platform store.

```json
{
  "schemaVersion": 4,
  "id": "sample-game",
  "name": "Sample Game",
  "game": {
    "executableName": "game.exe",
    "windowTitle": "Game Window"
  },
  "modules": [],
  "cooldowns": [
    {
      "id": "dash",
      "name": "Dash",
      "durationMs": 4000,
      "maxCharges": 1,
      "retrigger": "ignore",
      "recovery": "single"
    }
  ],
  "cooldownGroups": [],
  "panels": [
    {
      "id": "main-bar",
      "name": "Main",
      "layout": "row",
      "style": "bar",
      "bar": {
        "width": 360,
        "height": 30,
        "fillColor": "#50BAC8",
        "backgroundColor": "#202A31",
        "borderColor": "#80D8E8",
        "textColor": "#F1F5F7",
        "hideWhenReady": true
      },
      "anchorX": 0.5,
      "anchorY": 0.75,
      "cooldowns": ["dash"]
    }
  ],
  "bindings": [
    {
      "cooldownId": "dash",
      "chord": [
        { "device": "keyboard", "code": 16 },
        { "device": "keyboard", "code": 69 }
      ]
    }
  ],
  "hidBindings": []
}
```

`game` fields match conjunctively on both platforms: every specified field
must hold (`executableName` by case-folded equality, `executablePath` exactly
on Linux and case-folded on Windows, `windowClass` and `windowTitle` as
case-folded substrings), so the sample above needs both `game.exe` and a title
containing "Game Window". A profile with no identity field never matches.

Keyboard `code` values are Win32 virtual-key codes on both platforms
(`16` is Shift, `69` is E), so this binding fires on Shift+E. The schema-4
example requires the empty `modules`, `cooldownGroups`, and `hidBindings`
arrays; older schemas (1 and 2) are migrated when loaded. Existing canonical
legacy serialization is unchanged. `layerctl profile import-json` names the
first field it rejects.

### Overlay items (V2.7, in development)

Schema 5 adds optional cooldown `activeDurationMs` and a panel's ordered `items`
array. Unchanged legacy cooldown-only panels keep writing `cooldowns`; using
new typed features promotes the profile to schema 5. In schema 5, `items` is
authoritative when both panel lists are present; saving rewrites `cooldowns`
as the ordered projection of known cooldown-kind items, not a second list to
render. Unknown item kinds are preserved for round trips, skipped by rendering,
and reported in diagnostics rather than deleted or guessed.

Each known item has a stable `id`, a `kind`, and an optional `label`:

| Kind | Source and options |
| --- | --- |
| `cooldown` | `cooldownId`; existing remaining time and charge behavior |
| `timer` | `source: "automation"` with `timerId`, or `source: "cooldownActive"` with `cooldownId` |
| `counter` | Numeric `variable`; optional positive finite `max`; `display: "number"` or `"pips"` (pips require an integer `max` from 1 to 8) |
| `indicator` | Boolean, integer, or number `variable` (active when true or nonzero); `flashHz` is 0 (off), or 0.5–3 Hz in the editor (stored values up to 4 Hz load and save unchanged and flash at 3 Hz) |

For example, the following is a schema-5 panel's item list, not a complete
profile:

```json
{
  "items": [
    { "id": "dash-row", "kind": "cooldown", "cooldownId": "dash" },
    { "id": "dash-active", "kind": "timer", "source": "cooldownActive", "cooldownId": "dash" },
    { "id": "buff-timer", "kind": "timer", "source": "automation", "timerId": "buff" },
    { "id": "stacks-row", "kind": "counter", "variable": "stacks", "max": 8, "display": "pips" },
    { "id": "buff-indicator", "kind": "indicator", "variable": "buffActive", "flashHz": 2 }
  ]
}
```

A positive `activeDurationMs` on `dash` starts a separate active window whenever
that cooldown accepts a trigger; it needs no automation. Automation-backed
timers and variables are authored in the Automation editor. Missing sources or
wrong variable types display an unavailable row instead of a fabricated zero.
A declared automation timer that is not running is an ordinary stopped row;
an undeclared or removed timer ID is unavailable and reports a shared overlay
item diagnostic in both daemons and both editors. Declared variable defaults
can be presented without starting automation.
Typed profiles allow at most 64 panels and 1–32 items per typed panel.

Both editors provide an ordered item list with add, edit, reorder, and remove,
a cooldown active-duration control, and an automation-timer preset. Preview and
simulation use the same core presentation as the daemon. Native overlays,
Wayland/GNOME frames, and Game Bar consume resolved values and flash phases;
none independently evaluates automation or changes input behavior.

Windows sidecars use these directories beneath the profile root:

- `_visual/<id>.json` — visual calibration and template definitions (schema 2)
- `_automation/<id>.json` — declarative automation graphs (schema 1)
- `_companion/<id>.json` — Game Bar slot mappings and preferences (schema 1)
- `_state/<id>.json` — persistent automation variables (schema 1)

Saving canonicalizes sidecar JSON whitespace and object-key order once. Existing
sidecars remain readable; ordered visual observations and typed automation
values retain their meaning. Local IPC uses the same nested-document encoding
without quoting or base64-wrapping profile JSON.

Windows template images are content-addressed under `_visual/assets/`. Editing,
profile deletion, and calibration do not automatically reclaim them: undo/redo
and unapplied calibration can still own images absent from current sidecars.
Unused images therefore remain on disk. The internal offline-maintenance
operation requires all interactive owners to be closed and holds recovered
store writer ownership throughout its reference scan and deletion; it is not an
interactive cleanup command.

## Documentation

Each document has a narrow owner; open the one that matches the task instead of
treating the list as a required reading stack.

- [`docs/Linux.md`](docs/Linux.md) — Linux support, commands, packaging, permissions, and evidence.
- [`docs/DesktopEvidence.md`](docs/DesktopEvidence.md) — submitting desktop and hardware test evidence to pull requests.
- [`docs/GameBarCompanion.md`](docs/GameBarCompanion.md) — Game Bar architecture and invariants.
- [`docs/GameBarCompanion.audit.md`](docs/GameBarCompanion.audit.md) — V2.4 release-evidence record.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — contribution workflow, project boundaries, CI/release gates, desktop evidence, and PR rules.
- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) — vendored dependency notices.
