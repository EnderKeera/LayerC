# LayerC — Agent Guide and Project Memory

This file is the canonical agent-facing summary of the LayerC project: what it is, how it is
laid out, how to build and test it, the design rules that must not be broken, and the conventions
this repository uses. `README.md` and `PLAN.md` remain the authoritative user and product/plan
documents; when they disagree with this file, trust README/PLAN and fix this file.

## Project overview

LayerC is a lightweight, open-source **Windows cooldown overlay** (Windows 10 22H2 / Windows 11 x64),
written in dependency-free C++20 with native Win32 UIs (no Qt, no managed runtime). One process
(`layerd.exe`) detects matched game windows, consumes input passively, runs cooldown/automation state,
and draws click-through GDI overlay panels; a second process (`layere.exe`) is the profile editor;
`layercal.exe` is a short-lived calibration helper; `layerworker.exe` is the demand-launched isolated
visual-capture helper. The only vendored third-party code is miniz (ZIP) under `third_party/miniz`.

Versions: V1 (input+overlay), V1.1 (pixel predicates), V2.1 (template recognition), V2.2 (modular
runtime, generic HID, profile packs, isolated visual worker), V2.3 (bounded Automation Graphs,
cooldown recovery modes and groups), V2.4 (Game Bar companion work in progress; verify release
status against PLAN.md and the applicable review register).

**The product does not**: inject into games, read game memory, install drivers, require admin,
evade anti-cheat, synthesize gameplay input, or do any network access/telemetry. It treats input
purely passively. These are hard product constraints.

## Repository layout

| Path | Contents |
|---|---|
| `src/core/` | Platform-neutral core: typed profile model (`types.hpp`), JSON parser/serializer, cooldown engine (recovery modes, groups), input matcher, visual/template/pixel engines, automation AST+runtime, live-edit protocol, HID/hash/utf8 utilities. Headers under `src/core/include/layerc/core/`. |
| `src/platform/` | Win32 adapters: framed pipes, live channel, profile store + transaction journal, HID devices, text/UTF-8, visual assets, window discovery. Headers under `src/platform/include/layerc/platform/`. |
| `src/capture/` | Window capture: WGC primary backend, Desktop Duplication fallback, bounded readback math. |
| `src/runtime/` | Platform-neutral `DaemonKernel`, `CapabilityDemand`, `IRuntimeService`, `RuntimeServiceHost` (headers under `src/runtime/include/layerc/runtime/`). |
| `src/daemon/` | `layerd.exe`: sessions, input/HID/XInput, overlays, visual workers, isolated-helper protocol, live editing, automation service, diagnostics. |
| `src/editor/` | `layere.exe`: native Win32 editor (profiles, panels, cooldowns, visual calibration UI, packs, automation graph dialog + simulator). |
| `src/calibration/` | `layercal.exe`: calibration dialog launched by LayerE. |
| `src/bundle/` | `.layercprofile` packs: deterministic ZIP export/import/validation (miniz, LayerE-only). |
| `src/worker/` | `layerworker.exe`: isolated visual capture/evaluation helper. |
| `tests/` | CTest suites (see Build). `tests/test_support.*` is the canonical harness (`layerc::test::expect`). |
| `tools/` | `lctest` fake game window, `capture_probe`, benchmark scripts (`benchmark_v11/v22/v23/latency.ps1`), automation benchmark. |
| `examples/` | `lctest.json` (matches the lctest harness), `smoke.json` (global free-floating smoke profile). |
| `review/` | Layer review files (`layerN.review-YYYY-MM-DD.md`) and perf reports; gitignored working docs (see Review workflow). |
| `build/`, `out/` | Build outputs; never touched by hand, never committed. |

## Build, test, run

Requirements: Windows 10 22H2+ / Windows 11, Visual Studio 2026 with the Desktop C++ workload, CMake 3.28+.
MinGW builds are also supported (`mingw-ninja` preset, needs `MINGW_ROOT` and Ninja on PATH).

```powershell
cmake --preset vs2026              # configure
cmake --build --preset vs2026-debug
ctest --preset vs2026-debug        # all suites
cmake --workflow --preset vs2026-release   # configure+build+test+install+ZIP in one
cmake --install build/vs2026 --config Release --prefix out/layerc
```

The install stages the four executables (`layerd`, `layere`, `layercal`, `layerworker`). Close running
LayerE/LayerD before rebuilding so their executables are not locked. MinGW builds need the matching
runtime DLLs at deployment time (known pitfall: `libwinpthread-1.dll` resolving to Git's runtime
instead of the UCRT64 one).

Test targets: `layerc_core_tests` (profiles, cooldowns, automation, replay), `layerc_runtime_tests`
(capability-demand matrix, lazy ownership, rollback, 100 activation cycles), `layerc_capture_tests`
(deterministic capture math, headless-safe), `layerc_overlay_tests` (real HWND attachment via `lctest`),
`layerc_platform_tests` (store/transaction/peers), `layerc_editor_tests` (dialog semantics, undo/redo),
`layerc_bundle_tests` (packs), `layerc_isolated_visual_tests` (helper protocol). Interactive capture
tests only when configured with `-DLAYERC_ENABLE_INTERACTIVE_CAPTURE_TESTS=ON`.

Development: set `LAYERC_PROFILE_DIR` to load profiles from a disposable directory instead of
`%LOCALAPPDATA%\LayerC\profiles` (e.g. `$env:LAYERC_PROFILE_DIR = "$PWD\examples"`). Run `lctest`
as a fake game, then Shift+E in it triggers the `dash` cooldown from `examples/lctest.json`.

Release benchmarks (gates, not toys — they refuse false passes): `tools/benchmark_v11.ps1`
(Low/Balanced visual, 35 MiB / 50 MiB / 3% gate), `benchmark_v22.ps1` (empty daemon plateau and
recovery), `benchmark_v23.ps1` (automation activation/checkpoint/overhead, <=0.25 ms average and
<=2 ms max evaluator budget), `benchmark_latency.ps1` (<=1 ms median/p95 input-to-overlay regression).
Evidence lands in `out\benchmarks`.

## Architecture (what every change must respect)

### Layering and boundaries
- Platform-independent behavior lives in `core` and `runtime`; Win32 handles never cross into them.
  The editor UI types never reach the daemon. `platform` adapts Win32; `capture` implements capture
  backends; `daemon` owns process authority (sessions, cooldowns, overlays are daemon-authoritative).
- Shared data types: `GameProfile`, `GameMatch`, `Panel`, `CooldownDefinition`, `InputChord`,
  `GameSession`, `CooldownState`, `AutomationProfile`/`AutomationRuntime`. Use opaque IDs and
  normalized values across process boundaries (pipes carry framed UTF-8 JSON with size limits).
- Process boundary: LayerE and LayerD exchange versioned, length-prefixed JSON over a current-user-
  only named pipe; peers verify sibling executable, same user/logon session, and the exact same
  package version — mixed release binaries are rejected.

### V2.2 modular runtime
- `DaemonKernel` computes one immutable `CapabilityDemand` after every relevant state change;
  `RuntimeServiceHost` instantiates `IRuntimeService`s only while a session demands them and releases
  heavy resources in the same reconciliation cycle (no warm-cache grace period). Opening LayerE alone
  must not construct capture/template/HID/edit-overlay resources.
- Teardown order: producers first (visual/input), then overlays, then session state. Activation order:
  session state, input baseline seeding, overlays, then visual capture. Failure unwinds newly
  activated services in reverse order (see `RuntimeServiceHost::reconcile`).
- The daemon is event-driven: no idle polling loops, no fixed-rate daemon timers for visual capture
  (workers notify via events), no busy waits.

### V2.3 automation
- Automation is a bounded declarative system, NOT a scripting runtime: no text language, loops,
  recursion, user functions, filesystem/registry/network access, process launch, or code execution.
- Hard caps (named constants in `automation.hpp`): 64 rules, 64 variables, 32 timers per session,
  256 AST nodes, depth 16, 64 collection entries, 512-byte strings, 8192 node evaluations and
  32 derived events per root, 2 ms execution budget per root; 256 KiB sidecar, 64 KiB state file,
  4 MiB trace. Exceeding a cap fails closed with an `AutomationError`.
- Evaluation is transactional: one root event = one ordered pass over all enabled automations,
  priority resolves per-target conflicts, the winning set commits atomically through
  validator+committer callbacks, and an error faults only the offending automation (re-evaluated
  from the original snapshot); retry/reload/edit clears the fault.
- Persistent variables are shared per profile across sessions and checkpointed atomically under
  `profiles\_state` within 1 s of first change (final flush on teardown); machine-local state and
  traces are never exported in packs.

### Persistence model
- One UTF-8 JSON file per game profile; atomic replace + `<profile>.json.bak` last-valid backup.
  Visual data in `_visual\<profile>.json`, automation in `_automation\<profile>.json` (schema-1
  sidecar), persistent state in `_state`. A `.state` marker file is the authoritative removal/active
  signal for sidecars (crash-safe). Base schema 1-4; explicit Save upgrades older profiles after a
  compatibility warning; unknown schemas/module kinds are rejected.
- Multi-document writes (base + visual + automation) commit through one crash-recoverable journal
  transaction (`profile_transaction.cpp`): stage, durable journal, publish, durable commit marker,
  cleanup. Never write the profile and its sidecars in a way that can be left half-applied.
- Profile packs: deterministic ZIP (fixed timestamps, ordered entries), validated fully before any
  choice; limits 128 MiB archive/uncompressed, 256 entries, 64 profiles, 4 MiB per asset; rejects
  traversal/case-collisions/duplicates/bad hashes. Conflicts default to Import Copy. Data-only and
  unsigned.

### Capture and visual recognition
- WGC primary; Desktop Duplication is a foreground-only fallback (window-intersection copy, rotation
  transformed); no WARP/GDI fallback at runtime. Low/Balanced read back aspect-preserving 64/256 px
  grids; Exact reads every pixel with no guarantee. One newest-frame WGC pool (older frames dropped).
- Templates: <=4 states, <=4 references each, <=512x512, >=64 opaque pixels, flat/near-flat references
  rejected, PNG alpha = mask; coarse-to-fine bounded matcher with fixed per-frame work budget (increase
  stride, never unbounded work).
- Isolated helper: per-HWND `layerworker.exe` under a kill-on-close Job (1 process, 256 MiB commit),
  random pipe + nonce, mutual PID/path/session/version validation, 1 MiB message cap, sequence+QPC
  staleness rejection, 500 ms heartbeat, 2 s no-progress terminate + one retry, then quarantine
  (input/cooldowns keep working); Runtime Diagnostics can retry.

## Design rules (non-negotiable)

- **Passive input**: never block, modify, or synthesize user input.
- **Monotonic clocks** for all timers/cooldowns; wall-clock changes must not alter state.
- **Demand-driven resources**: no service/worker/HWND/capture device allocated without an active
  consumer; release in the same reconcile cycle; keep the daemon within the memory/CPU gates in
  `PLAN.md` (idle <=12 MiB private / 20 MiB WS / 0.2% CPU; visual 35/50/3%).
- **Fail closed**: invalid profiles/sidecars/messages/automations are rejected or faulted, never
  silently guessed; two invalid sidecar copies deliberately block triggers for that profile (fail-
  safe) while the session stays diagnosable.
- **Bounded everything**: every parse, packet, archive, AST, trace, and queue has an explicit size/
  count limit and rejects overflow (checked arithmetic, finite doubles, exact integers).
- **Atomicity**: files replace atomically with durable flush; related documents move as one
  transaction; derived events/effects commit only after the full set validates.
- **No busy loops, no polling where an event exists**: input, capture, live channel, and automation
  are event/wakeup-driven.
- **Determinism**: packs, benchmarks, and replay must be reproducible (fixed ZIP times, monotonic
  replay through the production evaluator).
- **Incremental behavior**: live edits diff profiles (`plan_session_reconciliation`); only a cooldown
  whose runtime definition changed is reset; sessions keep running timelines otherwise.

## Code conventions

- C++20, `-Wall -Wextra -Wpedantic` (MinGW) / `/W4 /permissive- /EHsc /utf-8` (MSVC). Keep files
  under ~1000 lines; split rather than sprawl (three new V2.3 files already exceed it — open finding).
- Errors: `std::optional`/`std::variant` results with structured error types (`ProfileError`,
  `AutomationError`, `PackOperationResult`); `[[nodiscard]]` on check-worthy returns. Win32 calls
  check failures and release handles on every path.
- `layerc::core` / `layerc::platform` / `layerc::daemon` / `layerc::editor` namespaces; internal
  helpers in anonymous namespaces; shared canonical helpers live in `core` (`json.cpp`, `text.cpp`,
  `hash.cpp`, `utf8.cpp`) — reuse them, do not re-implement (duplication is a standing review theme).
- JSON output is built with the `json::append_*` primitives; keep serializers byte-stable.
- Formatting: clang-format, 2-space indent (some legacy core files are 4-space — do not mix). Avoid
  wholesale re-wrapping of unchanged code in feature diffs (review hygiene).
- Threading: daemon logic stays on one message loop unless measured; worker threads are owned and
  joined; shared state is mutex-guarded and snapshots are value-copied at boundaries.

## Review workflow (this repository's process)

- Reviews live in `review/` (gitignored working docs, not committed). Use
  `layerN.review-YYYY-MM-DD.md` as the code-review filename convention; `layerN` is a
  placeholder, not a current layer number. When a review is requested, inspect the
  working tree and select the applicable consolidated register instead of relying on a
  hardcoded pointer in this guide.
- Findings use severity (blocker/major/medium/low/nit), category, evidence with `file:line`, and a
  concrete remedy. Each review re-verifies prior items as **FIXED / PARTIAL / REMAINING**.
- **Housekeeping rule**: when a review file is checked, every finding is verified against the current
  working tree. A file whose findings are ALL fixed is moved to `review/archiv/`. A file with open
  findings remains available and its open items are compiled — deduplicated, tagged with their source
  such as `[LN-B1]` — into the applicable consolidated review file.
- Release-gate status and prior-finding status live in the applicable review register. This guide
  intentionally does not embed a current layer number, archived range, or stale fix-pass summary.
  Re-run release gates after fix commits land.
  The un-merged detail from the 2026-08-14 audit is in `.review_tmp/CODE_REVIEW.md`.
- External review tooling: `reasonix.toml` is the checked-in config for the Reasonix CLI (external
  review/delegation). Per the workflow rules below it is only used when the user explicitly asks.
  When running Reasonix from an automated environment, beware leftover processes holding session
  leases under `%APPDATA%\reasonix\projects\d--oursoft-layerc` — use `--copy` to escape a held
  lease.

## Git hygiene

- **Never stage or commit legacy `.codewhale/` runtime state** unless the user explicitly asks.
- `review/`, `build/`, `out/`, `.review_tmp/` are ignored/working docs; keep them out of commits.
- The working tree may contain uncommitted milestone WIP; verify `git status` before assuming HEAD
  reflects the code; do not commit unrelated WIP as part of another change.
- Keep docs in pace with milestones: `README.md` (user features, benchmarks) and `PLAN.md` (canonical
  product/plan/gates) are updated by the same change that implements the feature.

## Agent workflow rules

- The primary agent implements, builds, tests, and fixes changes directly in this workspace.
- Do not use collaboration subagents (`spawn_agent`, `followup_task`, or equivalent) for
  implementation, review, or research. Keep implementation, review, research, builds, tests, and
  fixes within the current primary session.
- Do not invoke Reasonix, CodeWhale, or another external review/delegation agent unless the user
  explicitly requests it for the current task.
- Never stage or commit legacy `.codewhale/` runtime state unless the user explicitly requests it.

## Baseline project state (recorded 2026-08-17)

- V2.3 (automation core+runtime+daemon+editor, cooldown groups/recovery, schema 4, packs with
  automation modules) is COMMITTED as LayerC 2.3.0 after the V2.3 fix pass cleared all
  blockers; release gates verified same day: MSVC + MinGW Release builds, full CTest suites on
  both toolchains, benchmark v22 (plateau 1.9/11.3 MiB) and v23 (13.0 us avg / 1614 us max),
  install staging, and the `LayerC-2.3.0-windows-x64.zip` portable package.
- Known-good areas (recently verified): transaction journal, HidResolver cache, snapshot sequencer,
  origin-token dedup, chunked trace deltas, editor refactor shape, automation cascade deadline,
  persistent-value sync invariants, cooldown recovery delta detection.
- Before changing behavior in these areas, re-read the current files — line numbers in reviews drift
  as fixes land.

## References

- `README.md` — user-facing features, build/benchmark instructions, protocol/security notes.
- `PLAN.md` — canonical product plan, version gates, resource budgets, acceptance criteria.
- `THIRD_PARTY_NOTICES.md` — vendored components (miniz).
- `reasonix.toml` — external review CLI configuration (used only on explicit request).
- The applicable consolidated open-finding register under `review/`; review files use the `layerN.review-YYYY-MM-DD.md` convention.
