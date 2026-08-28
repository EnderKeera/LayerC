# LayerC

LayerC is a lightweight, open-source Windows cooldown overlay — dependency-free C++20 core, typed JSON profiles, demand-driven input and click-through panels, opt-in visual recognition, bounded Automation Graphs, native editor, and deterministic packs.

## Quickstart

The gate is CI on push. A main push → `actionlint` → `ci` preset (Ninja+sccache+UNITY) → CTest + `benchmark_v22` + `benchmark_v23` → package → `publish-rolling`. Hardware gates (`tools/run-hardware-benchmarks.ps1` on self-hosted `layerc-bench` — desktop with real GPU/display) auto-mirror evidence to `out/benchmarks` and staged install to `out/layerc`. No local build/benchmark needed for a normal push.

To iterate and debug locally, build once with the presets below, then run the
binaries from the build output:

```powershell
# Configure and build Debug + run tests (local iteration only)
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

CI artifacts and hardware evidence are mirrored to your working tree automatically; no download needed. Intervention only on gate failure or to start the runner (`run.cmd` interactive, never service). `tools/pull-ci.ps1` / `tools/push-and-test.ps1` are manual fallbacks; tag `v*.*.*` for durable releases. Local builds are for iteration only — never a gate.

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
| CI (actionlint + Ninja+sccache+UNITY) | `actionlint` → `cmake --preset ci` → `cmake --build --preset ci --parallel` → `ctest --preset ci` |
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

Benchmark scripts enforce strict hard gates under `tools/` and write JSON evidence to
`out/benchmarks/`. The V2.2 idle plateau and V2.3 CPU gates run in CI. The hardware-only
gates (V1.1 visual, V2.1 template, latency) run automatically on the interactive
`layerc-bench` runner — the workflow pins the checkout and artifact to one CI commit,
uploads evidence as
`benchmark-evidence` even when a gate fails, and mirrors the JSON into your working tree's
`out/benchmarks/`.

To run the same hardware gates **manually** (e.g. the runner is offline, or you want to
drive it yourself), authenticate `gh`, check out the CI run's `headSha`, and stage its
exact inputs:

```powershell
gh run download <run-id> -n layerc-windows-x64-tools -D out\bench-stage
gh release download baseline-v22 -D out\baseline --clobber
$baselineZip = Get-ChildItem out\baseline -Filter *.zip -File
if ($baselineZip.Count -ne 1) { throw "Expected one baseline ZIP." }
Expand-Archive $baselineZip.FullName -DestinationPath out\baseline -Force

.\tools\run-hardware-benchmarks.ps1 `
  -CandidateBuildDirectory .\out\bench-stage `
  -BaselineBuildDirectory .\out\baseline

# Optional visual-only pass (no baseline required)
.\tools\run-hardware-benchmarks.ps1 `
  -CandidateBuildDirectory .\out\bench-stage -VisualOnly

# V2.3 CPU evaluator only
.\tools\benchmark_v23.ps1 -BuildDirectory .\out\bench-stage
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
