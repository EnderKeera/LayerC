<!-- Keep the diff focused; preserve unrelated changes. -->

## Summary

<!-- What changed and why. Link issues with `Fixes #NNN` when applicable. -->

## Validation

<!-- What observable behavior did you check? Include the focused command/result. -->

```text
<!-- e.g. cmake --preset vs2026 / cmake --build --preset vs2026-debug / ctest --preset vs2026-debug -->
<!-- or: cmake -S . -B build-linux -G Ninja / cmake --build build-linux / ctest --test-dir build-linux -->
```

## Risk and completion

- [ ] The requested behavior and nearest relevant failure/recovery path are covered
- [ ] No unrelated files or generated `build*/`, `out/`, `review/`, `.review_tmp/`, `.codewhale/`, `.mimosa/`, or `.codegraph/` output is staged
- [ ] Affected user/platform documentation is accurate

<!-- Note only applicable risks: passive input, anti-cheat boundary, network/telemetry, monotonic timing, service demand, profile/bundle compatibility, manual hardware evidence, desktop evidence (docs/DesktopEvidence.md). -->
