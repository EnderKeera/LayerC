---
name: layerc-commit-to-ci-evidence
description: Bind LayerC local, CI, benchmark, package, and release evidence to a specific commit.
---

# LayerC commit-to-CI evidence

Select gates from the [README test/evidence sections](../../../README.md#test-targets), current [CMakeLists.txt](../../../CMakeLists.txt), and the affected workflow. Source and configuration own target names, thresholds, artifacts, and release dependencies.

## Select the cheapest meaningful local check

Map changed files to the executable/test target that actually compiles them. Inspect CMake sources and platform/option gates; a similarly named test may cover helpers rather than the application's implementation. Confirm registration with `ctest -N` for the chosen build/configuration, then build and run only the affected target(s).

For a configured Windows Debug tree, the shape is:

```powershell
cmake --build --preset vs2026-debug --target layerc_runtime_tests
ctest --preset vs2026-debug -R '^layerc_runtime_tests$' --output-on-failure
```

Replace the example target with the one justified by the change, and configure through the current preset only if needed. Use the configured Linux build directory and target for Linux checks. Parser/lint or reference validation may suffice for documentation/workflow-only changes. Do not run a full local build plus all tests merely to conclude a change.

## Bind remote evidence to the commit

1. Record branch, full commit SHA, and working-tree status. Distinguish local uncommitted changes from the code represented by a CI run.
2. Resolve an existing run with [resolve-ci-candidate.ps1](../../../tools/resolve-ci-candidate.ps1), passing the intended branch and full `-Commit`. Omitting the commit selects a latest successful run and is unsuitable for proving a particular change.
3. Inspect run/job conclusions and the artifact provenance. Reuse existing passing evidence; rerun only for a changed candidate, failure, or unresolved coverage gap.
4. Download necessary artifacts into a unique task directory under `out/`, recording run ID and SHA. Check contents against the current artifact contract before launching binaries.

[push-and-test.ps1](../../../tools/push-and-test.ps1) pushes by default. Use it only within existing push authorization. `-WatchOnly` implies no push; `-NoPush` still continues into downloads/hardware checks. Read its current clean-tree requirements and replacement of staging directories before use. For status-only work or a dirty checkout, use the resolver and read-only `gh run` commands directly rather than changing the checkout to satisfy the helper.

[pull-ci.ps1](../../../tools/pull-ci.ps1) stages the rolling release; do not use that moving release as proof of a different commit. Before any recursive staging cleanup, resolve and verify the paths and preserve relevant evidence.

## Benchmark and package branches

- **Windows resource/automation gates:** inspect [benchmark_v22.ps1](../../../tools/benchmark_v22.ps1) and [benchmark_v23.ps1](../../../tools/benchmark_v23.ps1) for workload, process prerequisites, thresholds, and output contracts. Prefer evidence already produced by the matching CI run.
- **Interactive visual/template/latency:** read [run-hardware-benchmarks.ps1](../../../tools/run-hardware-benchmarks.ps1). Supply an explicit candidate directory and, for latency comparison, an identified baseline. `-VisualOnly` omits latency; do not describe it as the full hardware gate. Do not kill unrelated running applications to satisfy benchmark prerequisites.
- **Linux:** use [Linux programs-per-environment verification](../layerc-linux-programs-per-environment/SKILL.md) for container reproduction and physical native/Flatpak evidence. Windows package success cannot establish Linux compatibility.
- **Game Bar:** read [GameBarCompanion.md](../../../docs/GameBarCompanion.md), [GameBarCompanion.audit.md](../../../docs/GameBarCompanion.audit.md), and the current packaging targets. Separate `layerc_companion_tests`, optional MSIX construction, actual widget/App Service operation, and manual/signing/certification evidence. Test stale snapshot and reconnect behavior when transport changes; check snapshot bounds and identity against current code. Package generation does not establish installation or certification.
- **Release consistency:** compare CMake version, README support/release claims, specialized docs, package contents, and actual workflow dependencies. Record mismatches; edit or publish only within the requested scope.

## Completion

Stop when the requested claim is backed by evidence from the same candidate. Report the SHA/build provenance, focused local results, relevant CI or benchmark artifacts, and manual/optional gates as passed, failed, unavailable, or not run with a reason.
