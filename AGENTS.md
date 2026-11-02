# LayerC — Agent Guide

Repository constraints for coding agents. LayerC is a C++20 cooldown overlay
with a shared core and native Windows and Linux backends. Read only the context
needed for the current task; tracked source, CMake, tests, and workflows override
memory and historical plans.

## Route context by task

| Task | Authoritative context |
| --- | --- |
| User behavior, support matrix, setup, programs, test targets, profile format | [`README.md`](README.md) |
| Linux behavior, CLI, dependencies, packaging, permissions, physical evidence | [`docs/Linux.md`](docs/Linux.md) |
| Linux editor plan history and dated local GUI/resource evidence | [`docs/LinuxEditorPlan.md`](docs/LinuxEditorPlan.md) |
| Editor visual design: tokens, typography, geometry, components, revert tags | [`docs/DesignSystem.md`](docs/DesignSystem.md) |
| Wayland backend architecture, contracts, and invariants (V2.7, in development) | [`docs/Wayland.md`](docs/Wayland.md) |
| Game Bar architecture and invariants | [`docs/GameBarCompanion.md`](docs/GameBarCompanion.md) |
| Game Bar release evidence | [`docs/GameBarCompanion.audit.md`](docs/GameBarCompanion.audit.md) |
| Build graph, options, versions, exact target registration | [`CMakeLists.txt`](CMakeLists.txt), test registration in [`cmake/LayerCTests.cmake`](cmake/LayerCTests.cmake), and current source |
| CI, benchmark, package, and release gates | [`CONTRIBUTING.md`](CONTRIBUTING.md#ci-and-release-gates), then the affected file under `.github/workflows/` and `tools/` |
| Desktop/hardware evidence reports, PR-comment workflow, trust tiers | [`docs/DesktopEvidence.md`](docs/DesktopEvidence.md) |

Local-only `archive/PLAN.md` (which includes V2.5) and
`archive/PLAN-v26-linux.md` are historical design records, not current
execution checklists, and may be absent. Archived one-off handoffs are
unverified references, not completion claims. Project skills under
`.agents/skills/` (and `.claude/skills/verify` for Linux runtime checks)
provide workflow-specific guidance; load them only for matching tasks.

## Non-negotiable product boundaries

- Input is passive: never block, modify, or synthesize it. XTest is test-only and
  must not link into `layerd`.
- No injection, hooks, game-memory reads, drivers, administrator elevation, or
  anti-cheat bypass.
- No telemetry or network access, with two exceptions: the same-package Game
  Bar App Service `LayerC.GameBar.v1`, and the Windows launcher `layerc.exe`,
  which may make anonymous HTTPS GETs to this repository's GitHub release at
  launch to update itself and the app, can be turned off by the user, and
  exits after starting `layerd`. `layerd`, `layere`, `layercal`, and `layerw`
  never access the network or link an HTTP client.
- Cooldowns, retries, deadlines, and timers use monotonic clocks.
- Optional services are demand-driven, idle-blocking, owned, and joined.
  Unavailable capabilities retain visible fault reasons without weakening
  unrelated features.
- Preserve profile compatibility and unsupported bindings. Move profiles between
  Windows and Linux through validated JSON or `.layercprofile` bundles, never by
  copying a platform store.
- Gamescope and Wayland are unsupported in V2.6; do not imply otherwise. The
  V2.7 Wayland backend is in development under
  [`docs/Wayland.md`](docs/Wayland.md); it makes no support claim until it
  lands. Its only global Wayland input source is passive evdev, opt-in and
  dependent on the user's own `input` group membership; uinput is test-only.
  Gamescope stays unsupported.

## Architecture boundaries

- Keep shared-core interfaces free of Win32/X11 handles and platform headers.
- Reuse core JSON, text, UTF-8, hashing, and validation. Emit byte-stable JSON
  through `json::append_*`; do not add platform-local serializers or duplicate
  normalization.
- Profile writes use the existing durable `stage -> commit` transaction,
  one-writer ownership, backup/journal markers, and crash recovery.
- Derive automation limits and schemas from the current parser and
  `src/core/include/layerc/core/automation.hpp`. Validate the complete winning
  effect set before committing external effects; persistent state remains under
  transactional `_state` storage.
- Shared mutable data is mutex-protected with value-copy boundaries. The daemon
  owns one platform event/message loop.

### Windows

Keep overlays non-activating and click-through. Visual capture uses WGC with
Desktop Duplication fallback and explicit unavailable/fault diagnostics. The
isolated worker remains authenticated, Job-bounded, kill-on-close, and
demand-launched. Game Bar-specific limits and authority live in
[`docs/GameBarCompanion.md`](docs/GameBarCompanion.md).

### Linux

Keep base profiles in XDG config, sidecars/state in XDG data, and authenticated
control sockets in `XDG_RUNTIME_DIR` or the private documented fallback. One X11
connection is shared by adapters and integrated into the blocking event loop.
XI2 input is read-only; Shape overlays stay hidden unless an empty input region
is verified. Composite/Damage capture uses the bounded documented fallback.
Headless operation retains store/control behavior while reporting graphical
capabilities unavailable. See [`docs/Linux.md`](docs/Linux.md) for exact
contracts.

## Repository conventions

- Use C++20 and keep MSVC/GCC/MinGW builds warning-free with the flags configured
  by CMake.
- Keep translation units focused and around 1000 lines or less.
- Prefer `optional`/`variant`, typed `*Error` values, and `[[nodiscard]]` for
  fallible returns.
- Use the existing `layerc::core`, `platform`, `capture`, `runtime`,
  `daemon`, `editor`, and `gamebar` namespaces as appropriate.
- Preserve unrelated user changes. Exclude `build/`, `build-*/`, `out/`,
  `review/`, `.review_tmp/`, `.codewhale/`, `.mimosa/`, `.codegraph/`, and
  `.pi/` from source analysis and never stage their generated content.
- Durable reviews use `review/layerN.review-YYYY-MM-DD.md` and move to
  `review/archiv/` only after every finding is verified fixed.

## Validation and evidence

Local tests use disposable fixtures and have no production access. Run the
smallest configured target that exercises the requested change, fix failures
caused by the change, and rerun it without pausing for approval. Documentation or
workflow-only edits may need only parser, link, or reference validation. Use a
full local build and all CTest suites only when the change has comparable breadth.
`ci.yml` on `main` pushes and pull requests is the authoritative cross-platform
gate on GitHub-hosted Windows and Linux runners, including fork pull requests.
Desktop and hardware evidence is advisory: pull request comment reports become
non-required checks through [`docs/DesktopEvidence.md`](docs/DesktopEvidence.md);
there are no self-hosted runners.

Resolve exact commands, targets, thresholds, artifact names, and registration
from current CMake, tests, tools, and workflows. Bind CI, benchmark, and package
claims to the exact commit. Keep automated, interactive-hardware, physical Linux,
signing, and certification evidence distinct; do not promote unobserved or
skipped coverage.

## Documentation changes

Update only the document that owns the changed behavior plus any summary that
became inaccurate. Keep version metadata, support claims, test tables, workflow
names, packaging details, and specialized documentation synchronized.

## Decision boundaries and completion

- Implement in the current task. The harness's persistent sidekick agent is
  pre-authorized: hand it mechanical implementation, builds, test runs, and
  environment setup with a self-contained brief that restates the relevant
  repository constraints, the files it owns, and the acceptance evidence to
  return. The primary agent settles the design; authors CI gate, benchmark
  threshold, and test-gate policy changes itself; reviews every sidekick change
  and its evidence before acceptance; and keeps commits, pushes, external
  mutations, and communication with the user.
- Other subagents, delegated runs, and multi-worker teams still require the
  user's explicit authorization for the current task; without it, perform the
  remaining work directly. If authorized, keep delegated work self-contained and
  preserve these repository constraints; the primary agent reviews it before
  acceptance. The user chooses the agent harness for each task; no particular
  launcher or orchestration tool is required. Give each worker a non-overlapping
  ownership boundary and acceptance evidence. `tools/start-orca-team.ps1` is an
  optional Orca workflow (see
  [`CONTRIBUTING.md`](CONTRIBUTING.md#optional-orca-agent-team)).
- When delegating through Pi, use only models in Pi's current scoped model
  list. Resolve that list before dispatch; do not hard-code stale model IDs or
  use an out-of-scope fallback. If no scoped model is available, stop and ask
  the user.
- Inspect source, tests, CMake, or specialized documentation when the requested
  change crosses that boundary—not as a universal preflight.
- Carry implementation through focused validation and the nearest relevant
  failure or recovery path.
- Stop for user input when completion requires an external mutation, unavailable
  authority, or a product choice that materially changes scope.
- Finish when the requested behavior is verified, affected documentation is
  accurate, temporary mutations are restored, and remaining platform or hardware
  gaps are reported explicitly.
