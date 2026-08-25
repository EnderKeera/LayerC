# LayerC

LayerC is a lightweight, open-source Windows cooldown overlay — dependency-free C++20 core, typed JSON profiles, demand-driven input and click-through panels, opt-in visual recognition, bounded Automation Graphs, native editor, and deterministic packs.

See the plan document (local-only; kept out of this repository) for roadmap
and technical specifications.

## Quickstart

```powershell
# Configure and build Debug + run tests
cmake --preset vs2026
cmake --build --preset vs2026-debug
ctest --preset vs2026-debug

# Full Release workflow: build, test, install to out/layerc, and create ZIP
cmake --workflow --preset vs2026-release
```

Installed executables land in `out/layerc/`:
- `layerd.exe` — background daemon (passive input, cooldown state, overlays)
- `layere.exe` — profile editor & calibration UI
- `layercal.exe` — visual calibration helper
 - `layerworker.exe` — isolated visual capture worker (demand-launched)

CI builds come from GitHub, not locally: push to `main` and the pipeline
builds, tests, benchmarks, and publishes the portable ZIP as a rolling
prerelease (tag `rolling`). `tools/pull-ci.ps1` downloads and stages it into
`out/layerc`; the benchmarks that need real hardware or a baseline (v11
visual, v22 idle plateau, latency) run on this machine against that build.
Tag `v*.*.*` for durable tagged releases. The local commands above are for
iteration and debugging.

### Development & Smoke Testing

```powershell
# Run with disposable example profiles
$env:LAYERC_PROFILE_DIR = "$PWD\examples"
.\build\vs2026\Debug\layerd.exe

# In another terminal, run the test harness (Shift+E triggers Dash cooldown)
.\build\vs2026\Debug\lctest.exe
```

## Building

### Requirements
- Windows 10 22H2+ or Windows 11 x64
- Visual Studio 2026 (Desktop C++ workload)
  - Optional Game Bar host requires **Universal Windows Platform tools** + **C++ UWP tools**
- CMake 3.28+

| Task | Command |
|---|---|
| Configure (VS 2026) | `cmake --preset vs2026` |
| Build Debug | `cmake --build --preset vs2026-debug` |
| Test Debug | `ctest --preset vs2026-debug` |
| Build Release | `cmake --build --preset vs2026-release` |
| Test Release | `ctest --preset vs2026-release` |
| Install | `cmake --install build/vs2026 --config Release --prefix out/layerc` |
| Package ZIP | `cmake --workflow --preset vs2026-release` |
| CI (Ninja+sccache+UNITY) | `cmake --preset ci` → `cmake --build --preset ci --parallel` → `ctest --preset ci` |
| MinGW Release *(optional cross-check)* | `cmake --workflow --preset mingw-ninja` (needs `MINGW_ROOT` & Ninja) |
| Game Bar MSIX | `cmake --build --preset vs2026-release --target layerc_gamebar_msix` (needs UWP tools) |

## Test Targets

| Target | Description |
|---|---|
| `layerc_core_tests` | Profile parsing, serialization, cooldown state machine, automation evaluator, replay |
| `layerc_runtime_tests` | Capability-demand matrix, lazy lifecycle, reconciliation, rollback, 100-cycle stress |
| `layerc_capture_tests` | Capture math, normalization, Low/Balanced/Exact bounds (GPU-independent) |
| `layerc_overlay_tests` | HWND attachment, GDI overlay rendering, capture exclusion, profile matching via `lctest` |
| `layerc_platform_tests` | Profile store, atomic transactions, backups, named pipe IPC, peer authentication |
| `layerc_editor_tests` | Editor dialog semantics, theme consistency, undo/redo history |
| `layerc_bundle_tests` | `.layercprofile` deterministic ZIP export/import, SHA-256 asset deduplication |
| `layerc_isolated_visual_tests` | `layerworker.exe` handshake, IPC protocol, message bounds, quarantine behavior |
| `layerc_companion_tests` | Game Bar companion sidecar, snapshot serialization, 2s stale expiration |
| `layerc_automation_benchmark` | CPU automation evaluation micro-benchmark ($\le 0.25\text{ ms}$ avg, $\le 2.0\text{ ms}$ max budget) |
## Release Benchmarks

Benchmark scripts enforce strict hard gates under `tools/` and write JSON evidence to `out/benchmarks/`:

```powershell
# V1.1 / V2.1 Visual Recognition (gate: <=35 MiB private, <=50 MiB WS, <=3% CPU)
.\tools\benchmark_v11.ps1 -Quality low
.\tools\benchmark_v11.ps1 -Quality balanced
.\tools\benchmark_v11.ps1 -Workload v21-template -Quality balanced

# V2.2 Modular Runtime (empty-profile plateau <=12 MiB private, <=20 MiB WS, 0% CPU)
.\tools\benchmark_v22.ps1 -BuildDirectory .\build\vs2026\Release

# Input-to-Overlay Latency (gate: <=1 ms median / p95 regression vs baseline)
.\tools\benchmark_latency.ps1 `
  -BaselineBuildDirectory .\out\layerc-vs2026-v22 `
  -CandidateBuildDirectory .\build\vs2026\Release

# V2.3 Automation Evaluator (gate: <=0.25 ms avg, <=2.0 ms max)
.\tools\benchmark_v23.ps1 -BuildDirectory .\build\vs2026\Release
```

## Features Overview

### Passive Cooldown Overlays (V1.0)
- Buffered Raw Input (keyboard, mouse) & XInput (up to 4 controllers).
- Non-activating, click-through, non-topmost Win32 GDI overlay windows attached to matched game HWNDs.
- Supports windowed and borderless-windowed modes. Never injects, hooks, or reads game memory.

### Visual Recognition (V1.1 – V2.1)
- Opt-in per cooldown: numeric predicates (color, brightness, coverage) and template matching (PNG references with alpha masks).
- Windows Graphics Capture (WGC) primary, Desktop Duplication fallback. Aspect-preserving downsampled readback grids (64px Low, 256px Balanced).
- Optional per-profile **Isolate capture** launches a separate sandboxed `layerworker.exe` (256 MiB job limit, kill-on-close).

### Modular Demand-Driven Runtime (V2.2)
- Zero idle resources: input, HID, overlays, visual capture, and live edit services instantiate only when demanded by an active session.
- Deterministic `.layercprofile` packs with miniz ZIP compression, SHA-256 deduplicated assets, and transactional commits.
- Generic HID support via Raw Input with slot mapping and descriptor hashing.

### Declarative Automation Graphs (V2.3)
- Bounded rule-based state graphs reacting to input, visual triggers, cooldowns, timers, and variables.
- Strict resource caps: 64 rules, 64 variables, 32 timers, 256 AST nodes, 2 ms execution budget per root event.
- Live editor canvas, deterministic offline simulator, and transactional state checkpointing under `profiles/_state/`.

### Xbox Game Bar Companion (V2.4)
- UWP XAML host (`layerc-gamebar.exe`) with 8 panel slots + settings widget.
- Same-package App Service `LayerC.GameBar.v1` with 64 KiB bounded snapshots (2s stale clear).
- Demand-driven transport — zero cost when unpackaged; settings via `_companion` sidecar. Implementation complete; remaining gates are manual matrix/signing (see `docs/GameBarCompanion.audit.md`).
## Configuration Format

Profiles are stored as UTF-8 JSON in `%LOCALAPPDATA%\LayerC\profiles\<name>.json`:

```json
{
  "schemaVersion": 4,
  "id": "sample-game",
  "name": "Sample Game",
  "game": {
    "executableName": "game.exe",
    "windowTitle": "Game Window"
  },
  "panels": [
    {
      "id": "main-bar",
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
      "cooldownIds": ["dash"]
    }
  ],
  "cooldowns": [
    {
      "id": "dash",
      "name": "Dash",
      "durationMs": 4000,
      "maxCharges": 1,
      "trigger": { "key": "E", "modifiers": ["Shift"] }
    }
  ]
}
```

Sidecars:
- `_visual/<name>.json` — visual calibration & template definitions (schema 2)
- `_automation/<name>.json` — declarative automation graphs (schema 1)
- `_companion/<name>.json` — Game Bar widget slot mappings & preferences (schema 1)
- `_state/<name>.json` — persistent automation variables (schema 1)
