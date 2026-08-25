# LayerC — Agent Guide and Project Memory

This file is the canonical agent-facing guide for LayerC: architecture, repository layout, non-negotiable design rules, build/test workflows, and coding conventions. `README.md` (user documentation) and `PLAN.md` (canonical product/plan/gates) are authoritative.

## Project Overview

LayerC is a lightweight, open-source **Windows cooldown overlay** (Windows 10 22H2 / Windows 11 x64) in dependency-free C++20 with native Win32 UIs (no Qt, no .NET).

### Core Components
- `layerd.exe` — background daemon: passive input, cooldown/automation state machines, click-through GDI overlays.
- `layere.exe` — native Win32 editor: profile management, live editing, automation canvas & simulator.
- `layercal.exe` — visual calibration helper launched from LayerE.
- `layerworker.exe` — isolated visual capture/matching worker (demand-launched).
- `third_party/miniz` — vendored miniz 3.1.2 for deterministic ZIP profile packs (the only third-party code).

### Product Invariants (Non-Negotiable)
- **Passive input only**: never block, modify, or synthesize keyboard/mouse/controller input.
- **No anti-cheat bypass**: no code injection, memory reading, driver installation, or admin elevation.
- **No telemetry/network**: zero network connections (except same-package Game Bar App Service IPC).
- **Monotonic timing**: all cooldowns and timers use monotonic clocks (`std::chrono::steady_clock`).

---

## Repository Layout

| Path | Description |
|---|---|
| `src/core/` | Platform-neutral core: profile models (`types.hpp`), JSON parser/serializer, cooldown engine, matcher, automation AST & runtime, live-edit protocol, hash/text/utf8 utilities. Headers in `src/core/include/layerc/core/`. |
| `src/platform/` | Win32 adapters: named pipes, live channel, profile store + transaction journal, HID devices, visual assets, window discovery. Headers in `src/platform/include/layerc/platform/`. |
| `src/capture/` | Capture backends: Windows Graphics Capture (primary), Desktop Duplication (fallback), aspect-preserving readback math. |
| `src/runtime/` | Platform-neutral runtime: `DaemonKernel`, `CapabilityDemand`, `IRuntimeService`, `RuntimeServiceHost`. Headers in `src/runtime/include/layerc/runtime/`. |
| `src/daemon/` | `layerd.exe`: session manager, input router, overlay window manager, visual worker manager, diagnostics. |
| `src/editor/` | `layere.exe`: Win32 UI, property inspector, zoomable automation canvas, timeline simulator. |
| `src/calibration/` | `layercal.exe`: screen region picker, color/brightness predicates, template reference editor. |
| `src/bundle/` | `.layercprofile` packs: deterministic ZIP packaging via miniz, SHA-256 asset deduplication. |
| `src/worker/` | `layerworker.exe`: isolated capture worker with kill-on-close Job object and 256 MiB commit limit. |
| `src/gamebar/` | Game Bar companion: UWP C++/WinRT host (`widget/`), App Service transport, snapshot bridge. |
| `tests/` | CTest test suites. `tests/test_support.*` provides canonical assertion helpers. |
| `tools/` | `lctest` fake game harness, `capture_probe`, benchmark scripts (`benchmark_v11/v22/v23/latency.ps1`). |
| `examples/` | `lctest.json` (matches `lctest.exe`), `smoke.json` (free-floating desktop test profile). |
| `build/`, `out/` | Build & install outputs (gitignored). Never touch by hand; never commit. |
| `review/` | Code review registers (gitignored). Archived reviews live under `review/archiv/`. |

### Analysis Input Policy
- Exclude `build/`, `out/`, `review/`, `.review_tmp/`, `.codewhale/`, and `.mimosa/` from broad codebase scans.
- Default deployable executables live in `out/layerc/layer*.exe`.
- Prefer explicit source globs (`src/**`, `tests/**`, `tools/**`, `examples/**`) over broad root queries.

---

## Build & Test Workflow

Requirements: Windows 10 22H2+ / Windows 11 x64, Visual Studio 2026 (Desktop C++ workload), CMake 3.28+.

```powershell
# Standard Debug build & tests
cmake --preset vs2026
cmake --build --preset vs2026-debug
ctest --preset vs2026-debug

# Full Release workflow (build, test, install to out/layerc, and emit portable ZIP)
cmake --workflow --preset vs2026-release

# Optional Game Bar UWP MSIX (requires UWP C++ tools)
cmake --build --preset vs2026-release --target layerc_gamebar_msix
```

### Key Test Suites
- `layerc_core_tests`: profile codec, cooldown state machine, automation evaluator, replay.
- `layerc_runtime_tests`: demand matrix, lazy service lifecycle, rollback, 100-cycle stress.
- `layerc_capture_tests`: capture math, downsampling bounds, format conversion (headless-safe).
- `layerc_overlay_tests`: HWND attachment, GDI rendering, capture exclusion via `lctest.exe`.
- `layerc_platform_tests`: atomic profile store, transaction recovery, named pipe IPC security.
- `layerc_editor_tests`: native dialog controls, undo/redo stack.
- `layerc_bundle_tests`: deterministic ZIP packs, SHA-256 asset hashing, import conflict handling.
- `layerc_isolated_visual_tests`: `layerworker.exe` IPC protocol, nonce auth, quarantine recovery.
- `layerc_companion_tests`: Game Bar snapshot codec, 2s stale data timeout, sidecar persistence.
- `layerc_automation_benchmark`: CPU automation evaluation micro-benchmark (gate: $\le 0.25\text{ ms}$ avg, $\le 2.0\text{ ms}$ max budget; run automatically via CTest).
### Release Benchmark Gates
Benchmark scripts in `tools/` output JSON to `out/benchmarks/` and refuse false passes:
1. **`benchmark_v11.ps1`**: Visual recognition (gate: $\le 35\text{ MiB}$ private, $\le 50\text{ MiB}$ WS, $\le 3\%\text{ CPU}$).
2. **`benchmark_v22.ps1`**: Modular runtime idle plateau ($\le 12\text{ MiB}$ private, $\le 20\text{ MiB}$ WS, $\le 0.2\%\text{ CPU}$, zero thread growth).
3. **`benchmark_latency.ps1`**: Input-to-overlay latency regression ($\le 1\text{ ms}$ median / p95 vs baseline).
4. **`benchmark_v23.ps1`**: Automation evaluator ($\le 0.25\text{ ms}$ avg, $\le 2.0\text{ ms}$ max budget).

---

## Architecture & Design Rules

### 1. Demand-Driven Services (V2.2)
- `DaemonKernel` computes an immutable `CapabilityDemand` on every session state transition.
- `RuntimeServiceHost` allocates heavy service resources (Raw Input, WGC capture, template matcher, live edit channel) **only while an active session demands them**.
- Opening LayerE connects the live channel but allocates **no** capture, HID, or overlay resources.
- Idle daemon: zero capture threads, zero polling timers, event-driven wakeups.

### 2. Declarative Automation Graphs (V2.3)
- Bounded rule-based engine — **never** a general-purpose scripting runtime.
- **Hard caps**: 64 rules, 64 variables, 32 timers, 256 AST nodes (max depth 16), 512-byte string values, 2 ms evaluation deadline per root event.
- Root events execute as one atomic transaction across all enabled graphs.
- Persistent variables checkpoint to `profiles/_state/<id>.json` within 1s; `.layerctrace` recordings are local-only and never exported in packs.

### 3. Storage & Transactional Persistence
- Base profile: UTF-8 JSON under `profiles/<id>.json` (schemas 1–4). Atomic replacement with `<id>.json.bak` fallback.
- Sidecars: `_visual/` (schema 2), `_automation/` (schema 1), `_companion/` (schema 1), `_state/` (schema 1).
- Multi-document saves commit through a crash-recoverable journal (`profile_transaction.cpp`): stage $\to$ journal $\to$ replace $\to$ commit marker $\to$ cleanup.
- `.state` marker files (`active` / `removed`) provide authoritative sidecar removal semantics.

### 4. Visual Capture & Template Matching
- Windows Graphics Capture (WGC) is primary; Desktop Duplication is fallback. No WARP or GDI runtime fallbacks.
- Readback grids are aspect-preserving and downsampled: Low (64px), Balanced (256px), Exact (full, unbenchmarked).
- Template matching is a bounded CPU coarse-to-fine pass ($\le 4$ states, $\le 4$ references each $\le 512\times512$, $\ge 64$ opaque pixels).
- Per-profile **Isolate capture** sandboxes WGC into `layerworker.exe` under a 256 MiB Job object.

### 5. Game Bar Companion (V2.4)
- Separate UWP C++/WinRT XAML host (`layerc-gamebar.exe`) communicating with packaged `layerd.exe` over App Service `LayerC.GameBar.v1`.
- LayerD remains the sole authority: pushes bounded 64 KiB immutable snapshots and accepts validated preference sidecars.
- Unpackaged desktop LayerD runs zero Game Bar code/threads.

---

## Code Conventions

- **Standard**: C++20. MSVC flags: `/W4 /permissive- /EHsc /utf-8`. MinGW flags: `-Wall -Wextra -Wpedantic`.
- **File size limit**: keep files under $\sim 1000$ lines; split cohesive subsystems across translation units rather than sprawling.
- **Errors**: use `std::optional` / `std::variant` with typed error structs (`ProfileError`, `AutomationError`, `PackOperationResult`); mark returns `[[nodiscard]]`.
- **Namespaces**: `layerc::core`, `layerc::platform`, `layerc::daemon`, `layerc::editor`, `layerc::gamebar`, `layerc::runtime`.
- **Helpers**: reuse shared primitives in `core/` (`json.cpp`, `text.cpp`, `hash.cpp`, `utf8.cpp`) — do not duplicate.
- **JSON**: serialize via `json::append_*` primitives for byte-stable output.
- **Threading**: daemon runs on a single main message loop; worker threads are owned and joined; shared data is mutex-guarded and value-copied at boundaries.

---

## DevOps & CI

- **CI workflow** (`.github/workflows/ci.yml`): runs on `windows-2022` with Ninja + MSVC (`ci` preset) $\to$ build $\to$ `ctest` 9/9 $\to$ install $\to$ CPack ZIP.
- **Dependabot** (`.github/dependabot.yml`): weekly GitHub Actions version bumps.
- **Release workflow** (`.github/workflows/release.yml`): triggers on `v*.*.*` tags $\to$ builds Release package $\to$ creates GitHub Release with ZIP.
- **Branch protection**: protect `main`, require `windows-msvc` CI status check, enforce linear history.

---

## Agent Workflow Rules

1. Primary agent implements, builds, tests, and fixes directly in this workspace without subagent delegation.
2. Never stage or commit `.codewhale/`, `.review_tmp/`, or `review/` working files.
3. Keep `README.md` and `PLAN.md` in sync with feature and milestone changes.
4. Verify tests pass (`ctest --preset vs2026-debug`) and builds remain warning-free before concluding tasks.
5. Reviews live under `review/layerN.review-YYYY-MM-DD.md`; move to `review/archiv/` once all findings are marked FIXED.
