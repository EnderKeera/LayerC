# LayerC

[![CI](https://github.com/EnderKeera/LayerC/actions/workflows/ci.yml/badge.svg)](https://github.com/EnderKeera/LayerC/actions/workflows/ci.yml) ![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg) ![Windows](https://img.shields.io/badge/Windows-10_22H2%2B_%7C_11-blue.svg) ![Linux](https://img.shields.io/badge/Linux-X11_%2F_headless-lightgrey.svg)

LayerC is a lightweight, open-source cooldown overlay with a dependency-free
C++20 core and native platform backends. It provides passive input tracking,
click-through cooldown panels, opt-in visual recognition, bounded automation,
typed JSON profiles, and deterministic `.layercprofile` bundles.

Windows is the complete desktop experience. Linux V2.6 is a CLI-first X11
implementation with a headless fallback; its physical desktop evidence is still
pending. LayerC has no telemetry and never injects input, reads game memory, or
requires elevation.

## Support and release status

The version in `CMakeLists.txt` is the package/release version. Milestone labels
below describe implementation scope; they do not make an unreleased milestone a
published release.

| Surface | State in this tree | Release boundary |
| --- | --- | --- |
| Windows desktop (`layerd`, `layere`, `layercal`, `layerw`) | Implemented through the V2.5 feature set | Windows CI, benchmarks, install, and ZIP packaging are the release path |
| Xbox Game Bar companion (V2.4) | Host, App Service, widgets, transport, and persistence implemented | Manual matrix, certification, and production signing remain; see [`docs/GameBarCompanion.audit.md`](docs/GameBarCompanion.audit.md) |
| Linux portable runtime (V2.6.0) | Headless daemon, control CLI, XDG store, bundles, and packaging implemented | Container CI covers automated behavior |
| Linux X11 input/overlay/capture (V2.6.1–V2.6.2) | Implemented with XI2, Shape, RandR, Composite, Damage, and XFixes | Physical GNOME Xorg/KDE X11 native and Flatpak evidence is pending; see [`docs/Linux.md`](docs/Linux.md) |
| Gamescope and Wayland | Not supported | Deferred beyond V2.6 |

Package metadata currently reports **2.4.0**. Treat the V2.5 and V2.6 surfaces
as pre-release until version metadata and their stated evidence gates are
updated together.

## Non-negotiable guarantees

- **Passive input only:** LayerC reads input events; it never blocks or
  synthesizes them. XTest is linked only into a Linux test binary.
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
Universal Windows Platform and C++ UWP tools.

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
# Run the daemon with disposable example profiles
$env:LAYERC_PROFILE_DIR = "$PWD\examples"
.\build\vs2026\Debug\layerd.exe

# In another terminal: Shift+E triggers the example Dash cooldown
.\build\vs2026\Debug\lctest.exe
```

### Linux

Requirements: a C++20 compiler, CMake 3.28+, and Ninja or Make. X11 development
libraries enable input, overlays, capture, and `layercal`; without them LayerC
builds a headless store/control daemon.

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
| `layerc` | yes | no | Per-user launcher with opt-in signed updates; starts the installed daemon and exits (v2.6b, not yet shipped) |
| `layerd` | yes | yes | Cooldown/automation daemon; background desktop app on Windows, foreground service on Linux |
| `layere` | yes | no | Native profile editor, live edit, automation editor, and simulator |
| `layercal` | GUI | CLI on X11 | Visual calibration helper or bounded PPM/JSON snapshot tool |
| `layerw` | yes | no | Demand-launched isolated WGC capture worker with a 256 MiB Job limit |
| `layerctl` | no | yes | Status, reload, stop, and transactional profile/bundle import/export |
| `layerc-gamebar` | optional MSIX | no | Xbox Game Bar widget host and same-package App Service |

## Windows launcher and updates (v2.6b)

The portable Windows ZIP starts LayerC via `layerd.exe`. With an empty
`LAYERC_UPDATE_PUBLIC_KEY`, it omits `layerc.exe`; the launcher is packaged only
once a release key is provisioned. The launcher and its tests are still built
unconditionally.

`layerc.exe` installs its launcher per user under
`%LOCALAPPDATA%\Programs\LayerC`, registers Apps & Features and a Start menu
shortcut, starts the already-installed `layerd`, and exits. Autostart is a
separate opt-in (`--autostart=on|off`).

**Self-updates are OFF by default.** Production release signing is not yet
provisioned: this tree has no production public key, no release-signing secret
is configured, and the release workflow publishes ZIPs rather than signed
update assets. An unset, unreadable, or non-DWORD update preference stays OFF.
With updates OFF, the launcher skips the update path entirely, performs no
network I/O, and still starts the installed daemon. Keep an existing install
or run the packaged `layerd.exe` directly; an offline launcher cannot download
a first build.

The preference is `HKCU\Software\LayerC\Updates` (REG_DWORD); a nonzero value
opts in, zero or deleting the value opts out. In PowerShell:

```powershell
# Opt in for subsequent launches, only after signed releases are provisioned
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

To provision signing, the release owner generates an ECDSA P-256 key pair
outside the repository (`openssl ecparam -name prime256v1 -genkey -noout -out
<private-key-path>`), commits only the uncompressed public point (130 hex
characters starting `04`) to `packaging/windows/update-signing-key.hex`, or
supplies it through `LAYERC_UPDATE_PUBLIC_KEY` at CMake configure time.
The future signing job must use the PEM private key in the repository Actions
secret **`LAYERC_UPDATE_SIGNING_KEY`**, sign the exact `layerc_manifest` output
with `openssl dgst -sha256 -sign`, and publish `manifest.json`, its detached
DER `manifest.sig`, and the SHA-256-named file assets. This is not wired into
`release.yml` yet; do not enable unsigned updates. See
[release signing instructions](CONTRIBUTING.md#release-signing-windows-updates)
for key extraction, configuration, signing, and publication details.

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
`layercal`. The optional systemd user unit is default-off; Linux package details
live in [`docs/Linux.md`](docs/Linux.md).

## Capabilities

### Shared core

- Typed, versioned UTF-8 JSON profiles and sidecars.
- Cooldown state machines with charges and ignore/restart retrigger policies.
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
- Native editor, live preview, simulator, diagnostics, resilient executable-name
  matching, and optional exact-path matching.
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
- Click-through Shape overlays with RandR monitor scaling.
- Composite/Damage capture with a bounded direct-window fallback.
- `layerctl` profile/bundle transfer compatible with Windows JSON and
  `.layercprofile` payloads.
- No Linux GUI editor, tray, Wayland backend, or Gamescope support in V2.6.

### Xbox Game Bar companion

- Eight fixed panel slots and one settings widget.
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
| `layerc_launcher_tests` | Launcher update engine: signed manifest checks, delta downloads, atomic build switch, repair, and rollback (v2.6b, not yet shipped) |
| `layerc_runtime_tests` | Capability demand, lazy lifecycle, reconciliation, rollback, and stress |
| `layerc_capture_tests` | Capture math, normalization, and Low/Balanced/Exact bounds |
| `layerc_automation_benchmark` | Automation evaluator budget (`<= 0.25 ms` average, `<= 2.0 ms` p99); runs serially in CTest; maximum is reported, not gated |
| `layerc_benchmark_result_tests` | Benchmark result-writer contract (registered when PowerShell is available) |
| `layerc_visual_asset_gc_contract_tests` | No automatic collection from production edit/calibration paths; recovered store ownership for offline maintenance |

### Windows

| Target | Coverage |
| --- | --- |
| `layerc_launcher_win_tests` | Windows launcher: CNG signature verification, default-OFF update preference and opt-in/out, per-user registration/autostart in disposable HKCU keys, and the Start menu shortcut (v2.6b, not yet shipped) |
| `layerc_capture_integration_tests` | Interactive WGC/DD integration; registered only with `LAYERC_ENABLE_INTERACTIVE_CAPTURE_TESTS=ON` |
| `layerc_overlay_tests` | HWND attachment, GDI rendering, capture exclusion, and `lctest` matching |
| `layerc_platform_tests` | Profile store, transaction recovery, named-pipe IPC, and peer authentication |
| `layerc_daemon_runtime_tests` | Capability-service reconciliation, rollback, and demand-driven lifecycle |
| `layerc_bundle_tests` | Deterministic bundle export/import and SHA-256 asset deduplication |
| `layerc_isolated_visual_tests` | `layerw` protocol, bounds, authentication, and quarantine behavior |
| `layerc_companion_tests` | Companion sidecars, snapshots, transport, and stale expiration |
| `layerc_editor_tests` | Editor dialogs, theme behavior, schema helpers, and automation presets |
| `layerc_calibration_tests` | Calibration window layout, behavior, and worker interaction |

### Linux

| Target | Coverage |
| --- | --- |
| `layerc_linux_profile_store_tests` | XDG roots, permissions, journaling, fault injection, and recovery |
| `layerc_linux_profile_pack_tests` | Byte-deterministic bundle export and import roundtrip |
| `layerc_linux_event_loop_tests` | Monotonic timerfd deadlines, signal shutdown, and zero idle wakeups |
| `layerc_linux_control_channel_tests` | Unix-socket framing, peer UID, stale sockets, and `layerctl` commands |
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
prerelease; `v*.*.*` tags publish a release. Both ship only the Windows x64
ZIP; Linux V2.6 has no packaged artifact yet. Jobs, runners, artifacts, and
hardware benchmarks are described in
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

Keyboard `code` values are Win32 virtual-key codes on both platforms
(`16` is Shift, `69` is E), so this binding fires on Shift+E. Schema 4 requires
the empty `modules`, `cooldownGroups`, and `hidBindings` arrays; older schemas
(1 and 2) are migrated when loaded. `layerctl profile import-json` names the
first field it rejects.

Windows sidecars use these directories beneath the profile root:

- `_visual/<id>.json` — visual calibration and template definitions (schema 2)
- `_automation/<id>.json` — declarative automation graphs (schema 1)
- `_companion/<id>.json` — Game Bar slot mappings and preferences (schema 1)
- `_state/<id>.json` — persistent automation variables (schema 1)

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
- [`docs/GameBarCompanion.md`](docs/GameBarCompanion.md) — Game Bar architecture and invariants.
- [`docs/GameBarCompanion.audit.md`](docs/GameBarCompanion.audit.md) — V2.4 release-evidence record.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — contribution workflow, CI/release gates, hardware runner, and PR rules.
- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) — vendored dependency notices.
