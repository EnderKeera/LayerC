# LayerC Game Bar Companion (V2.4)

## Overview

The Game Bar companion displays LayerC cooldowns inside the Xbox Game Bar overlay as eight fixed panel slots (`PanelSlot1`–`PanelSlot8`) and a settings widget (`Settings`). It is distributed exclusively as an optional full MSIX package, not in the portable ZIP.

### Invariants
- **LayerD is authoritative**: the widget does not capture frames, process input, or mutate cooldown state.
- **Bounded & immutable**: snapshots are capped at 64 KiB and 512-byte text fields.
- **Stale data hiding**: snapshots older than 2.0 seconds are cleared.
- **Zero unpackaged cost**: portable desktop LayerD instantiates zero Game Bar services or threads.

---

## 1. Data Model (`layerc/core/companion.hpp`)

- `CompanionSlotMapping`: slot index (0..7), `panelId`, `pinned`, `clickThrough`, `opacity` ($[0.1, 1.0]$).
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

- **`GameBarBridge`** (`src/gamebar/GameBarBridge.cpp`): formats immutable snapshots from active cooldown states, caches the latest snapshot under a mutex, and enforces the 2.0s staleness cutoff.
- **`GameBarTransport`** (`src/gamebar/GameBarTransport.cpp`): demand-driven App Service client.
  - Active only in the packaged MSIX build when an active game session exists.
  - Connects to same-package App Service `LayerC.GameBar.v1`.
  - Sends bounded snapshots on game scans/overlay updates and drains preference updates via a single wait-handle event.
  - Reconnects with capped exponential backoff (100 ms to 2000 ms) after daemon/Game Bar/Explorer restarts.

---

## 4. UWP Widget Host (`layerc-gamebar.exe`)

- Built from `src/gamebar/widget/LayerC.GameBar.vcxproj` (C++/WinRT + XAML).
- **`App`**: handles `ms-gamebarwidget:` protocol activations and routes `PanelSlot1`–`PanelSlot8` and `Settings` to their respective XAML pages. Hosts the `LayerC.GameBar.v1` App Service endpoint.
- **`PanelWidgetPage`**: renders cooldown progress bars, remaining time, and charge counters; honors Game Bar theme, opacity, compact mode, pinning, and click-through.
- **`SettingsWidgetPage`**: UI to map each of the 8 slots to available panels, set opacity sliders, and toggle click-through; sends validated preference documents back to LayerD.
- **`WidgetSnapshotHub`**: in-process snapshot bus that validates incoming JSON, fans out to widget CoreWindows, and clears stale widgets after 2.0 seconds.

---

## 5. MSIX Packaging (`src/gamebar/Package.appxmanifest`)

The package manifest registers:
- 8 $\times$ standard panel widget extensions + 1 $\times$ settings widget extension.
- Same-package App Service `LayerC.GameBar.v1`.
- Full-trust process launcher for `layerd.exe`.
- In-package proxy stubs and private Game Bar WinMD metadata (`Microsoft.Gaming.XboxGameBar.Private.winmd`).
- Packaged binaries: `layerc-gamebar.exe`, `layerd.exe`, `layere.exe`, `layercal.exe`, `layerw.exe`, and VC runtime DLLs.
