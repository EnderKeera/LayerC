# LayerC Game Bar Companion (V2.7, in development)

## Overview

This document owns the companion architecture and invariants. Use
[`GameBarCompanion.audit.md`](GameBarCompanion.audit.md) only for release
evidence and [`../README.md`](../README.md) for project-wide status.

The Game Bar companion displays LayerC panels inside the Xbox Game Bar overlay
as eight fixed slots (`PanelSlot1`–`PanelSlot8`) and a settings widget
(`Settings`). V2.7 adds cooldown, timer, counter, and indicator rows. It remains
an optional full MSIX package, not part of the portable ZIP.

> **Release boundary:** V2.7 is a local development implementation, not a
> published or physically qualified release. The manual compatibility matrix,
> certification evidence, and production signing remain outstanding. The
> [V2.4 release-evidence record](GameBarCompanion.audit.md) is historical scoped
> evidence, not qualification of these changes or a claim that a signed package
> has shipped.

### Invariants

- **LayerD is authoritative**: the widget does not capture frames, process input, or mutate cooldown state.
- **Bounded & immutable**: snapshots are capped at 64 KiB; each label and value text field is limited to 512 UTF-8 bytes.
- **Stale data hiding**: snapshots older than 2.0 seconds are cleared.
- **Zero unpackaged cost**: portable desktop LayerD instantiates zero Game Bar services or threads.

---

## 1. Data Model (`layerc/core/companion.hpp`)

- `CompanionSlotMapping`: slot index (0..7), `panelId`, `pinned`, `clickThrough`, `opacity` ($[0.0, 1.0]$).
- `CompanionProfile`: schema-1 sidecar containing a list of `CompanionSlotMapping` entries for a profile. Its schema and eight-slot limit do not change.
- `CompanionSessionSnapshot`: immutable wire message with `profileId`, `sessionKey`, `timestampMs`, active `panels` (up to 8 panels $\times$ 32 items), catalog `availablePanels` (up to 64), and `preferences`.
- Legacy cooldown-only snapshots without item-scoped effects omit `snapshotVersion` (version 1) and retain their byte-stable cooldown format. Their `progress` is fraction elapsed toward ready ($0.0 = \text{triggered}, 1.0 = \text{ready}$).
- Typed panels, or any valid item-scoped automation effect (including an
  `@legacy:<index>` target on a cooldowns-only panel), select
  `snapshotVersion: 2`. Foreign-panel and out-of-range item targets never
  change the version. `CompanionTypedItem` carries `itemId`, known `kind`, separate `label`/`valueText`, resolved `fill` in $[0,1]$, `hasProgress`, pip counts (at most eight), 24-bit `color`, `active`, `flashOn`, and `unavailable`. Panels also carry `visible` and `omittedItems`.
- Shared `present_panel` resolves automation values, item overrides, unavailable states, and the session's monotonic flash epoch before publication. Whole-row text overrides do not retain a second stale value. The widget never reevaluates these inputs.
- Unknown profile item kinds remain preserved in the profile and are omitted from presentation with diagnostics. Unknown wire kinds/versions, duplicate IDs, and invalid wire bounds reject the incoming snapshot instead of guessing a rendering.

---

## 2. Persistence (`profiles/_companion/<id>.json`)

- Slot mappings persist under `profiles/_companion/<id>.json` (schema 1).
- Committed as part of the unified crash-recoverable profile transaction (base + visual + automation + companion).
- Validation rejects duplicate slots, duplicate panels, unknown panel IDs, or invalid opacity values.

---

## 3. Daemon Bridge & Transport

- **`core::build_companion_snapshot_report`** (`src/core/companion_snapshot.cpp`): builds bounded immutable snapshots from shared presentation and returns JSON plus an omitted-item count; `build_companion_snapshot` remains the JSON-only wrapper. All eight mapped panels retain their slot identity. Short-field 8 × 32-item snapshots fit; maximum-length/escaped text is not assumed to fit. When necessary the builder removes unmapped catalog detail, then sheds rows from the last panels while counting omissions. It never truncates inside a UTF-8 string. If even metadata cannot fit, it publishes no snapshot and retains a visible diagnostic. The daemon also reports omitted rows. The widget independently clears stale data after 2.0 seconds.
- **Companion-owned presentation timer**: the report also returns the next producer update its own visible rows need (`CompanionSnapshotBuild::next_wakeup`). A flashing indicator asks for its next half-period edge (capped at 3 Hz: 167 ms half periods, at most six transitions per second); a running cooldown or countdown asks for at most 100 ms so its text stays current. Static counters, steady indicators, hidden or unmapped panels, panels whose typed rows were shed, and legacy version-1 snapshots request no timer, and no deadline metadata is written on the wire. `CompanionHost` owns one Win32 timer (`companion_snapshot_timer`) on the daemon message window, re-arms it from each successful publication, reports it in `resources().timers`, and cancels it on quiescence, on a failed transport publication and on the next empty or hidden build. A quiesced companion therefore cannot resurrect its timer, and native overlay scheduling is not involved at all.
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
- **`PanelWidgetPage`**: renders legacy cooldowns or typed rows with wrapped labels and separate values, optional progress bars, up to eight pips, indicator swatches, unavailable states, and hidden/omitted diagnostics. Eight 8-DIP pips with 3-DIP gaps occupy 85 DIP. Flash-off phases preserve row geometry. It honors Game Bar theme, opacity, compact mode, pinning, and click-through. `WidgetRowModel.hpp` keeps row interpretation platform-neutral and testable; it is not a second automation evaluator.
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
