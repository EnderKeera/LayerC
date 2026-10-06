# LayerC Game Bar Companion (V2.4)

## Overview

This document owns the companion architecture and invariants. Use
[`GameBarCompanion.audit.md`](GameBarCompanion.audit.md) only for release
evidence and [`../README.md`](../README.md) for project-wide status.

The Game Bar companion displays LayerC cooldowns inside the Xbox Game Bar overlay as eight fixed panel slots (`PanelSlot1`–`PanelSlot8`) and a settings widget (`Settings`). It is distributed exclusively as an optional full MSIX package, not in the portable ZIP.

> **Release boundary:** the implementation described here is complete, but the
> manual compatibility matrix, certification evidence, and production signing
> remain outstanding. See the scoped
> [V2.4 release-evidence record](GameBarCompanion.audit.md). This document is the
> architecture contract, not a claim that a signed package has shipped.

### Invariants

- **LayerD is authoritative**: the widget does not capture frames, process input, or mutate cooldown state.
- **Bounded & immutable**: snapshots are capped at 64 KiB and 512-byte text fields.
- **Stale data hiding**: snapshots older than 2.0 seconds are cleared.
- **Zero unpackaged cost**: portable desktop LayerD instantiates zero Game Bar services or threads.

---

## 1. Data Model (`layerc/core/companion.hpp`)

- `CompanionSlotMapping`: slot index (0..7), `panelId`, `pinned`, `clickThrough`, `opacity` ($[0.0, 1.0]$).
- `CompanionProfile`: schema-1 sidecar containing a list of `CompanionSlotMapping` entries for a profile.
- `CompanionSessionSnapshot`: immutable wire message with `profileId`, `sessionKey`, `timestampMs`, active `panels` (up to 8 panels $\times$ 32 items), catalog `availablePanels` (up to 64), and `preferences`.
- `progress`: fraction elapsed toward ready ($0.0 = \text{triggered}, 1.0 = \text{ready}$).

---

## 2. Persistence (`profiles/_companion/<id>.json`)

- Slot mappings persist under `profiles/_companion/<id>.json` (schema 1).
- Committed as part of the unified crash-recoverable profile transaction (base + visual + automation + companion).
- Validation rejects duplicate slots, duplicate panels, unknown panel IDs, or invalid opacity values.

---

## 3. Daemon Bridge & Transport

- **`core::build_companion_snapshot`** (`src/core/companion_snapshot.cpp`): formats and serializes bounded immutable snapshots from active cooldown states, shrinking oversized live data before publication. The widget independently clears stale data after 2.0 seconds.
- **`GameBarTransport`** (`src/gamebar/GameBarTransport.cpp`): demand-driven App Service client.
  - Active only in the packaged MSIX build when an active game session exists.
  - Connects to same-package App Service `LayerC.GameBar.v1`.
  - Sends bounded snapshots on game scans/overlay updates and drains preference updates via a single wait-handle event.
  - Envelope keys (`kind`, `json`, `accepted`, `error`) are shared with the widget through `src/gamebar/GameBarEnvelope.hpp`; a rejected snapshot's `error` reason is kept in the transport's last-error diagnostic.
  - Reconnects with capped exponential backoff (100 ms to 2000 ms) after daemon/Game Bar/Explorer restarts.

---

## 4. UWP Widget Host (`layerc-gamebar.exe`)

- Built from `src/gamebar/widget/LayerC.GameBar.vcxproj` (C++/WinRT + XAML).
- **`App`**: handles `ms-gamebarwidget:` protocol activations and routes `PanelSlot1`–`PanelSlot8` and `Settings` to their respective XAML pages. Hosts the `LayerC.GameBar.v1` App Service endpoint.
- **`PanelWidgetPage`**: renders cooldown progress bars, remaining time, and charge counters; honors Game Bar theme, opacity, compact mode, pinning, and click-through.
- **`SettingsWidgetPage`**: UI to map each of the 8 slots to available panels, set opacity sliders, and toggle click-through; sends validated preference documents back to LayerD.
- **`WidgetSnapshotHub`**: validates incoming canonical JSON and owns one connection-scoped App Service session (connection, task, deferral, and event revocations). Superseded requests and terminal callbacks cannot mutate a newer session. Notifications are invalidations: each widget reads a synchronized, monotonic-fresh value copy on its dispatcher. Callbacks and resource cleanup run outside state mutexes. Page-owned subscriptions cover navigation, unload, close, and destruction; queued work cannot outlive its subscription. The hub clears stale widgets after 2.0 seconds.

---

## 5. MSIX Packaging (`src/gamebar/Package.appxmanifest`)

CMake configures this manifest from `project(LayerC VERSION ...)` into the build tree (including the four-part MSIX identity version). The widget MSBuild project and package staging both consume that generated manifest, and the generated `layerc/core/version.hpp` supplies the matching runtime identity.

The package manifest registers:

- 8 $\times$ standard panel widget extensions + 1 $\times$ settings widget extension.
- Same-package App Service `LayerC.GameBar.v1`.
- Full-trust process launcher for `layerd.exe`.
- In-package proxy stubs and private Game Bar WinMD metadata (`Microsoft.Gaming.XboxGameBar.Private.winmd`).
- Packaged binaries: `layerc-gamebar.exe`, `layerd.exe`, `layere.exe`, `layercal.exe`, `layerw.exe`, and VC runtime DLLs.

## Related documentation

- [`GameBarCompanion.audit.md`](GameBarCompanion.audit.md) — recorded automated evidence and outstanding manual/certification gates.
- [`../README.md`](../README.md) — project-wide support and release status.
