# LayerC — Agent Guide

Canonical agent guide. `README.md` (user docs) is authoritative; `PLAN.md` (roadmap/gates) is a local-only document, not tracked in the public repo. Repo state overrides memory.

## Project Overview

Lightweight **Windows cooldown overlay** — dependency-free C++20, native Win32 (no Qt/.NET). Windows 10 22H2 / Windows 11 x64.

- `layerd.exe` — daemon: passive input, cooldown/automation, click-through GDI overlays.
- `layere.exe` — editor: profile CRUD, live edit, automation canvas + simulator.
- `layercal.exe` — calibration helper (launched from editor).
- `layerworker.exe` — isolated WGC capture worker (demand-launched, 256 MiB Job, kill-on-close).
- `third_party/miniz` — vendored miniz 3.1.2, only third-party code.

**Invariants**
- Passive input only — never block/synthesize input.
- No anti-cheat bypass — no injection, memory read, driver, or admin elevation.
- No telemetry/network — except same-package Game Bar App Service (`LayerC.GameBar.v1`).
- Monotonic timing — `steady_clock` for all cooldowns/timers.

## Repository Layout

| Path | Description |
|---|---|
| `src/core/` | Platform-neutral core: `types.hpp`, JSON, cooldown engine, matcher, automation AST/runtime, companion, hash/text/utf8. Headers `src/core/include/layerc/core/` |
| `src/platform/` | Win32 adapters: pipes, live channel, `profile_store` + journal, HID, visual assets, window discovery. Headers `src/platform/include/layerc/platform/` |
| `src/capture/` | WGC (primary) + Desktop Duplication (fallback), aspect readback math |
| `src/runtime/` | `DaemonKernel`, `CapabilityDemand`, `IRuntimeService`, `RuntimeServiceHost`. Headers `src/runtime/include/layerc/runtime/` |
| `src/daemon/` | `layerd.exe` |
| `src/editor/` | `layere.exe` |
| `src/calibration/` | `layercal.exe` |
| `src/bundle/` | `.layercprofile` ZIP via miniz, SHA-256 dedup. `pack_detail.*` + `pack_sidecar_table.hpp` |
| `src/worker/` | `layerworker.exe` |
| `src/gamebar/` | UWP C++/WinRT host `widget/` + App Service bridge. Manifest `Package.appxmanifest` |
| `tests/` | CTest suites, `test_support.*` helpers |
| `tools/` | `lctest` harness, `capture_probe`, `benchmark_v11/v22/v23/latency.ps1` → `out/benchmarks/`; orchestrators `pull-ci.ps1` (rolling → `out/layerc`) and `push-and-test.ps1` (push → watch → pull → hardware gates) |
| `examples/` | `lctest.json`, `smoke.json` |
| `build/`, `out/` | Build/install outputs (gitignored, never commit). `build/` ~9 GB — delete after push |
| `review/` | Review registers (gitignored), `review/archiv/` archived |

Analysis: exclude `build/`, `out/`, `review/`, `.review_tmp/`, `.codewhale/`, `.mimosa/`. Prefer explicit `src/**` `tests/**` `tools/**` `examples/**` globs; default deployables `out/layerc/layer*.exe`.

## Build & Test

Req: Windows 10 22H2+ / 11 x64, VS 2026 (Desktop C++), CMake 3.28+.

```powershell
# Debug — local iterative
cmake --preset vs2026
cmake --build --preset vs2026-debug
ctest --preset vs2026-debug

# Release — local full package
cmake --workflow --preset vs2026-release  # build → test 10/10 → install out/layerc → CPack ZIP

# CI preset — what GitHub Actions uses (Ninja + MSVC + sccache + UNITY)
cmake --preset ci
cmake --build --preset ci --parallel
ctest --preset ci

# MinGW optional cross-compiler check (no UWP host) — not a release gate
cmake --workflow --preset mingw-ninja

# Game Bar MSIX (needs UWP tools)
cmake --build --preset vs2026-release --target layerc_gamebar_msix
```

Presets: `vs2026` (VS 2026), `mingw-ninja` (MinGW-w64 + Ninja), `ci` (Ninja + `sccache` launchers + `UNITY_BUILD` batch 16). After push, free space: `Remove-Item -Recurse -Force build, out -ErrorAction SilentlyContinue` (keep `out/benchmarks` if needed for evidence).

### Test Suites

| Suite | Covers |
|---|---|
| `layerc_core_tests` | profile codec, cooldown state, automation eval, replay |
| `layerc_runtime_tests` | demand matrix, lazy lifecycle, rollback, 100-cycle stress |
| `layerc_capture_tests` | capture math, downsample bounds, format conversion |
| `layerc_overlay_tests` | HWND attach, GDI, exclusion (`lctest.exe`) |
| `layerc_platform_tests` | atomic store, journal recovery, named pipe security |
| `layerc_editor_tests` | dialog controls, undo/redo |
| `layerc_bundle_tests` | deterministic ZIP, SHA-256, import conflicts |
| `layerc_isolated_visual_tests` | worker IPC, nonce, quarantine |
| `layerc_companion_tests` | snapshot codec, 2s stale, sidecar persistence |
| `layerc_automation_benchmark` | CPU eval ($\le 0.25$ ms avg, $\le 2.0$ ms max) — also CTest |

### Benchmark Gates (hard, `tools/*.ps1` → `out/benchmarks/*.json`)

1. `benchmark_v11.ps1` — visual: $\le 35$ MiB private, $\le 50$ MiB WS, $\le 3\%$ CPU
2. `benchmark_v22.ps1` — idle plateau: $\le 12$ MiB private, $\le 20$ MiB WS, $\le 0.2\%$ CPU, zero thread growth
3. `benchmark_latency.ps1` — input→overlay $\le 1$ ms median/p95 vs baseline
4. `benchmark_v23.ps1` — automation: $\le 0.25$ ms avg, $\le 2.0$ ms max

Run headful on integrated + discrete GPU where required; `out/benchmarks` is the evidence dir.The V2.3 gate runs in GitHub CI; the v11/v22/latency gates run on the self-hosted bench runner (see DevOps). Run headful on integrated + discrete GPU where required; `out/benchmarks` is the evidence dir.

## Architecture Rules

### 1. Demand-Driven Services (V2.2)
`DaemonKernel` → immutable `CapabilityDemand` each session transition. `RuntimeServiceHost` allocates Raw Input / WGC / matcher / live channel **only when demanded**. Opening LayerE allocates nothing. Idle daemon = zero capture/polling threads.

### 2. Declarative Automation (V2.3)
Bounded engine, never scripting. Caps: 64 rules, 64 vars, 32 timers, 256 AST nodes (depth $\le 16$), 512-byte strings, 2 ms/root-event deadline. Root event = atomic transaction. Vars checkpoint to `profiles/_state/<id>.json` ≤1s; `.layerctrace` never packed.

### 3. Storage & Transactions
Base UTF-8 JSON `profiles/<id>.json` (schemas 1–4) + sidecars `_visual/` (2), `_automation/` (1), `_companion/` (1), `_state/` (1). Atomic `.bak` fallback. Multi-doc saves via journal `stage → journal → replace → commit → cleanup`. `.state` marker (`active`/`removed`) for deletion.

### 4. Visual Capture & Matching
WGC primary, Desktop Duplication fallback (no WARP/GDI). Grids Low 64px / Balanced 256px / Exact full (unbenchmarked). Matching: bounded coarse-to-fine ≤4 states × ≤4 refs ≤512×512, ≥64 opaque. Isolate mode sandboxes WGC in `layerworker.exe` (256 MiB Job).

### 5. Game Bar Companion (V2.4)
Separate UWP XAML host (`layerc-gamebar.exe`) via App Service `LayerC.GameBar.v1`. LayerD authoritative — pushes bounded 64 KiB snapshots (2s stale clear), accepts validated `_companion` sidecar. Unpackaged LayerD = zero Game Bar code/threads.

## Code Conventions

- C++20, `/W4 /permissive- /EHsc /utf-8` (MSVC), `-Wall -Wextra -Wpedantic` (MinGW).
- Files ≤~1000 lines; split TUs instead of sprawling.
- Errors: `optional`/`variant` + typed `*Error` (`[[nodiscard]]`).
- Namespaces: `layerc::core/platform/daemon/editor/gamebar/runtime`.
- Reuse `core/` primitives (`json`, `text`, `hash`, `utf8`) — don't duplicate.
- JSON via `json::append_*` for byte-stable output.
- Threading: daemon = single message loop; workers owned/joined; shared data mutex + value-copy at boundaries.

## DevOps & CI

- **CI** `.github/workflows/ci.yml`: `windows-2022` + `ilammy/msvc-dev-cmd` + `seanmiddleditch/gha-setup-ninja@v6` + `mozilla-actions/sccache-action` (`SCCACHE_GHA_ENABLED` — sccache keeps its own Actions cache; no manual `actions/cache`). Steps: configure `ci` → build → `ctest 10/10` → `benchmark_v23` → install → `cpack` → upload `out/layerc` + ZIP and the `layerc-windows-x64-tools` artifact → publish rolling `rolling` prerelease ZIP on every green main push (`tools/pull-ci.ps1` pulls it). Never cache `build/ci` (stale `CMakeCache` breaks Ninja). Free tier: 10 GB cache, 7-day expiry.
- **Hardware Benchmarks** `.github/workflows/benchmarks.yml`: on green main `CI` (`workflow_run`) or manual dispatch, runs on the repo's self-hosted `[self-hosted, windows, x64, layerc-bench]` runner (this PC; register via `config.cmd \`--labels self-hosted,windows,x64,layerc-bench\`` and run `run.cmd` interactively — **not** a Windows service, the GUI gates need the interactive desktop; no local compile required). Steps: download `layerc-windows-x64-tools` artifact from the CI run → fetch `baseline-v22` release (frozen V2.2 binaries) → v22 plateau → v11 low/balanced/template → latency A/B vs baseline → upload `benchmark-evidence` (JSON → `out/benchmarks/`). Jobs wait up to 24 h for an offline runner, so it can be started on demand; `--ephemeral` supported.
- **PR Agent** `.github/workflows/pr-agent.yml` + `.pr_agent.toml`: `The-PR-Agent/pr-agent@v0.43.0` on `pull_request` (opened/reopened/synchronize/ready_for_review) and `issue_comment`, `DEEPSEEK_API_KEY` + `OPENAI_API_BASE=https://api.deepseek.com`, model `deepseek-v4-flash-vision-exp` → `v4-flash`.
- **Dependabot** `.github/dependabot.yml`: weekly Actions bumps.
- **Release** `.github/workflows/release.yml`: on `v*.*.*` tags → build Release → GitHub Release + ZIP.
- Branch protection: `main` linear, require `windows-msvc`.

## Agent Workflow

1. Implement inline — no subagent delegation (vibe mode forbids spawning; gate with `vs2026-debug` build + `ctest`).
2. Never stage/commit `.codewhale/`, `.review_tmp/`, `review/` working files. Avoid unscoped globs ( `build/` is 10k files ); use `git ls-files` and `read :range`.
3. Keep `README.md` + `PLAN.md` + `docs/GameBarCompanion*.md` in sync with feature/gate changes.
4. Warning-free + `ctest 10/10` before concluding. Use monotonic `steady_clock` for timers.
5. Reviews `review/layerN.review-YYYY-MM-DD.md` → `review/archiv/` when all FIXED. `build/`/`out/` gitignored — don't `git add`.
