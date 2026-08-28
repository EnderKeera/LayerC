# LayerC — Agent Guide

Canonical agent guide. `README.md` (user docs) is authoritative; repo state overrides memory.

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
- Monotonic timing — `steady_clock` or Win32 `GetTickCount64` for all cooldowns/timers.

## Repository Layout

| Path | Description |
|---|---|
| `src/core/` + `src/platform/` | Core (`types.hpp`, JSON, cooldown, automation, companion, hash) + Win32 adapters (pipes, live channel, `profile_store`+journal, HID, visual) |
| `src/capture/` + `src/runtime/` | WGC/DD fallback, aspect math + `DaemonKernel`/`CapabilityDemand`/`RuntimeServiceHost` |
| `src/daemon/` `src/editor/` `src/calibration/` `src/worker/` `src/gamebar/` | Binaries `layerd`/`layere`/`layercal`/`layerworker`/UWP host (`widget/`+App Service, `Package.appxmanifest`) |
| `src/bundle/` | `.layercprofile` ZIP (miniz), SHA-256 dedup, `pack_detail.*` |
| `tests/` `tools/` `examples/` | CTest suites, `lctest`/`capture_probe`/`benchmark_*.ps1`→`out/benchmarks/`, `lctest.json` |
| `build/` `out/` `review/` | Gitignored outputs (`build/` not needed for gate, `out/benchmarks`/`out/layerc`), `review/archiv/` |

Analysis: exclude `build/`, `out/`, `review/`, `.review_tmp/`, `.codewhale/`, `.mimosa/`. Prefer explicit `src/**` `tests/**` `tools/**` `examples/**` globs; default deployables `out/layerc/layer*.exe`.

## Build & Test

Req: Windows 10 22H2+ / 11 x64, VS 2026 (Desktop C++), CMake 3.28+.

> **The gate is CI on push, not a local build.** The presets below are for local
> iteration and debugging only. Do not run a full local build + ctest gate before
> concluding a change; the release gate is the GitHub `windows-msvc` job that builds,
> runs CTest, `benchmark_v22`, and `benchmark_v23` on every push. Build locally when you need
> to iterate or debug, not to validate.

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

### Test Suites & Benchmark Gates
See `README.md`##Test Targets / ##Release Benchmarks and `tools/*.ps1` (`benchmark_v11/v22/latency/v23`→`out/benchmarks/*.json`). V2.2/V2.3 gates run in CI; v11/latency on self-hosted `layerc-bench` (headful, integrated+discrete GPU).

## Architecture Rules
See `README.md`##Features Overview for V1.0–V2.4 caps. Enforcement: `src/runtime/DaemonKernel` (demand-driven, zero idle), `src/core/automation_*` (64/64/32/256 caps, depth ≤16, 2 ms, `_state` checkpoint), `src/platform/profile_store` (journal `stage→commit`, `.bak`, `.state` marker), `src/capture/wgc_backend` (WGC/DD, 64/256 grids, ≤4×4×512), `src/gamebar/GameBarBridge` (64 KiB, 2s stale, `_companion` sidecar). Detail in `docs/GameBarCompanion.md`.

## Code Conventions

- C++20, `/W4 /permissive- /EHsc /utf-8` (MSVC), `-Wall -Wextra -Wpedantic` (MinGW).
- Files ≤~1000 lines; split TUs instead of sprawling.
- Errors: `optional`/`variant` + typed `*Error` (`[[nodiscard]]`).
- Namespaces: `layerc::core/platform/daemon/editor/gamebar/runtime`.
- Reuse `core/` primitives (`json`, `text`, `hash`, `utf8`) — don't duplicate.
- JSON via `json::append_*` for byte-stable output.
- Threading: daemon = single message loop; workers owned/joined; shared data mutex + value-copy at boundaries.

## DevOps & CI
- **CI** `ci.yml`: `workflow-lint` (actionlint 1.7.12) → `windows-msvc` (`ci` Ninja+sccache+UNITY, CTest+`benchmark_v22`+`benchmark_v23`, artifacts). `publish-rolling` on `main` only (`contents:write`). Never cache `build/ci`.
- **Hardware** `benchmarks.yml`: `workflow_run` on green main or manual, pins `runId`/`headSha`, runs `run-hardware-benchmarks.ps1` on `[self-hosted,windows,x64,layerc-bench]` (`run.cmd` interactive), pulldown+mirror to `out/`; `if:always()` upload, 24h wait.
- **PR Agent** `pr-agent.yml`+.pr_agent.toml: `pr-agent@v0.43.0`, DeepSeek `v4-flash`, C++20/Win32/invariants focus.
- **Dependabot** weekly Actions, **Release** `v*.*.*`→ZIP, branch `main` linear `windows-msvc` required.

## Agent Workflow

1. Implement inline — no subagent delegation (vibe mode forbids spawning). **Do not run a local build + ctest gate.** The gate is CI on push: the `windows-msvc` job compiles, runs CTest, and gates `benchmark_v22` and `benchmark_v23`. Worktree stays uncommitted/unbuilt until you choose to push; pushing is the validation.
2. Never stage/commit `.codewhale/`, `.review_tmp/`, `review/` working files. Avoid unscoped globs; use `git ls-files` and `read :range`.
3. Keep `README.md` + `docs/GameBarCompanion*.md` in sync with feature/gate changes.
4. Write warning-free code (`/W4 /permissive-`) and use monotonic `steady_clock` or Win32 `GetTickCount64` for timers — but let CI confirm it. Do not run a local CTest pass just to conclude.
5. Reviews `review/layerN.review-YYYY-MM-DD.md` → `review/archiv/` when all FIXED. `build/`/`out/` gitignored — don't `git add`.
