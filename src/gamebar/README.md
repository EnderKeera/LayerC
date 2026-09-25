# LayerC Game Bar companion package

This local page routes package work; it does not duplicate architecture or
release status.

- Architecture and invariants: [`docs/GameBarCompanion.md`](../../docs/GameBarCompanion.md)
- Release evidence and remaining manual gates: [`docs/GameBarCompanion.audit.md`](../../docs/GameBarCompanion.audit.md)
- Project-wide support status: [`README.md`](../../README.md)

The optional full MSIX contains the same-package `LayerC.GameBar.v1` App
Service, eight panel widgets, one settings widget, immutable 64 KiB snapshots,
and a two-second stale-data cutoff.

```powershell
cmake --build --preset vs2026-release --target layerc_gamebar_msix
```

The target requires Visual Studio UWP and C++ UWP tools. A successful unsigned
build is not equivalent to completing the manual matrix or production signing.
