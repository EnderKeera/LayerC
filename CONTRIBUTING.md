# Contributing to LayerC

Thanks for helping with LayerC. This page routes contributors to the source that
owns each decision instead of duplicating the project contract.

## Choose the relevant reference

| Change | Read |
| --- | --- |
| Setup, programs, supported surfaces, profile format, test targets | [`README.md`](README.md) |
| Architecture, invariants, code conventions, validation boundaries | [`AGENTS.md`](AGENTS.md) |
| Linux adapters, CLI, packaging, or physical evidence | [`docs/Linux.md`](docs/Linux.md) |
| Game Bar transport, widgets, or MSIX | [`docs/GameBarCompanion.md`](docs/GameBarCompanion.md) |
| Exact build graph or CI behavior | current `CMakeLists.txt` or affected workflow |

Open only the references your change needs. Tracked source, tests, CMake, and
workflows resolve stale documentation.

## Work on a change

Preserve unrelated worktree changes and keep generated `build/`, `out/`,
`review/`, `.review_tmp/`, and `.codewhale/` content out of commits. Use
disposable profiles under `examples/` for mutation-heavy local checks.

The non-negotiable boundaries in [`AGENTS.md`](AGENTS.md) apply to every
change: passive input, no anti-cheat bypass, no telemetry/network beyond the
same-package Game Bar service and the launch-time `layerc.exe` updater, monotonic timing, demand-driven services, and
durable profile compatibility.

Run the smallest configured check that exercises the behavior you changed and
rerun failures caused by the change. CI owns the full cross-platform gate; use a
full local build and every suite only for work with that breadth. Resolve current
commands and targets from [`README.md`](README.md), CMake, and the affected
test registration.

## Pull requests

Explain what changed and why, the observable result you checked, and any
remaining manual, hardware, or platform gap. Update the owning documentation
when behavior, support, packaging, or release gates changed.

A contribution is ready when:

- the requested behavior and nearest relevant failure or recovery path are
  checked;
- the diff is focused and contains no generated output;
- compatibility or migration effects are documented; and
- user-facing and specialized documentation remains accurate.

## Reporting issues

Use the bug or feature template. Include only relevant environment details,
minimal reproduction steps, expected and actual behavior, and useful diagnostics.
Sanitize profiles, bundles, traces, and screenshots before attaching them.
