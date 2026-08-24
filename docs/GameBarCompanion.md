# LayerC Game Bar Companion (v2.4)

## Overview

The Game Bar companion renders LayerC cooldown state inside Xbox Game Bar as
eight fixed panel slots (`PanelSlot1`–`PanelSlot8`) plus a settings widget
(`Settings`). It is **not** part of the portable ZIP; it ships only in the
separate full MSIX package. See `docs/GameBarCompanion.audit.md` for release
status and `src/gamebar/README.md` for the package layout.

**Key invariants** (must never be violated):
- LayerD remains the sole source of truth for cooldown state.
- No capture, input processing, or cooldown mutation happens in the companion.
- Snapshots are bounded (64 KiB) and immutable; stale data (>2 s) is hidden.
- All measurements treat companion cost separately from native LayerD budgets.

## 1. Data model (`src/core/include/layerc/core/companion.hpp`)

- `CompanionProfile` — schema-1 sidecar: `profileId` + a bounded set of
  `CompanionSlotMapping` entries (0..7 slots) mapping a slot to a panel
  `panelId`, with `pinned`, `clickThrough`, and `opacity` overrides.
- `CompanionSessionSnapshot` — bounded immutable snapshot: `profileId`,
  `sessionKey`, `timestampMs`, up to 8 panels (each up to 32 cooldown rows with
  progress/charges/ready/remaining), the bounded panel catalog
  `availablePanels` (up to 64 entries), and current `preferences`. `progress`
  is the fraction elapsed toward ready (1.0 = ready, 0.0 = just triggered);
  `icon`/`color` and panel `visible` are reserved fields the bridge does not
  yet populate. Snapshots additionally enforce that each live panel matches its
  declared preference slot for the same panel id.
- All documents carry a 64 KiB bound and a 512-byte text bound; wire size is
  enforced after serialization too (oversized plausible metadata is rejected as
  `std::nullopt` and never published).

## 2. Persistence (sidecar, `src/platform/profile_store.cpp`)

- Slot mappings persist per profile under `profiles/_companion/<profile>.json`
  (base schema 4, companion module schema 1) through the same crash-recoverable
  bundle transaction as base/visual/automation.
- Saving an unrelated base/visual/automation edit preserves an existing valid
  companion sidecar; an invalid existing sidecar fails the bundle closed.
- Writes are validated by `layerc::core::validate_companion_profile` (duplicate
  slot, duplicate panel, unknown panelId, out-of-range opacity).

## 3. Bridge (`src/gamebar/GameBarBridge.cpp`)

`layerc::gamebar::GameBarBridge` formats an immutable `CompanionSessionSnapshot`
from session cooldown/panel state (`create_session_snapshot`), publishes the
panel catalog and slot preferences inside the snapshot, keeps one latest snapshot
under a mutex, and hides data older than the 2.0 s cutoff (`latest_snapshot` /
`latest_snapshot_json`). It is not WinRT and owns no connection.

`sessionKey` (wire field `CompanionSessionSnapshot::session_key`) is a bounded,
opaque runtime discriminator: `"global"` for global profiles and
`"window:<uintptr_t HWND>"` for window-bound sessions
(`GameBarBridge::make_session_key`). The `(profileId, sessionKey)` pair lets a
future widget distinguish per-window state. Consumers must not parse or persist
the representation. This replaces the pre-2.4
`wide_to_utf8(source_key)` file-path key — a breaking wire change for any
external consumer built against the old path-based key.

## 4. Transport (`src/gamebar/GameBarTransport.cpp`, daemon-owned)

- In the **full MSIX**, the daemon owns a same-package **App Service**
  connection named `LayerC.GameBar.v1`. It sends bounded immutable snapshot JSON
  (64 KiB) on each game-scan timer and on overlay updates, and receives bounded
  presentation-preference documents (`kind="preferences"`) back from the widget.
  Outside the packaged build (ordinary portable ZIP), `GameBarTransport` is a
  **zero-resource no-op**: construction owns no handle or WinRT object, and an
  unpackaged start fails closed without allocating either. Packaged wake/App
  Service resources exist only while `companion_sessions_` is nonempty.
- The widget is verified by same-package family identity; the daemon rejects
  forged preference requests (name/publisher/family+version checks mirror the
  manifest, and updates are only accepted on an open generation/epoch).
  `companion_sessions_` is decoupled from input focus so opening Game Bar does
  not clear the last published game session; the transport reconnects with
  capped exponential backoff after daemon/Game Bar/Explorer restarts.
- `GameBarRuntimeService` starts/polls/stops the transport on demand.
- Companion cost is accounted separately from the native-overlay budget.

## 5. Host (`src/gamebar/widget/LayerC.GameBar.vcxproj`, `layerc-gamebar.exe`)

- The host is a **UWP XAML App Service provider**: handle `ms-gamebarwidget:`
  activation in `OnActivated` (check `ActivationKind::Protocol` and
  `XboxGameBarWidgetActivatedEventArgs`), create and retain a per-window
  `XboxGameBarWidget(widgetArgs, CoreWindow, Frame)`, `Frame::Navigate` to the
  correct XAML page, `Window::Current().Activate()`, and clean up on
  `Window::Closed`. `OnBackgroundActivated` owns the `LayerC.GameBar.v1`
  background `AppServiceConnection`, queues at most 16 bounded preference
  requests under lock, and signals the daemon's wait handle; completion is
  deferred (`Deferral`) and cleared atomically when LayerD consumes the update
  and writes a validated sidecar bundle within its profile transaction.
- `PanelWidgetPage` (8 instances, ids `PanelSlot1`–`PanelSlot8`) renders at most
  one slot's cooldowns: progress bar, remaining, and charge count. It honors
  `RequestedTheme` / `RequestedOpacity` × `slot.opacity` (a deferred write to
  the sidecar), `CompactModeEnabled` (hides the status hint), and
  `ClickThroughEnabled ∧ slot.clickThrough` (via `IsHitTestVisible`), and hides
  stale or disconnected snapshots (`SnapshotHub`'s 2.0 s timer clears the page
  to "LayerD disconnected"; the daemon bridge independently rejects stale data
  from `latest_snapshot`).
  The settings button subscribes `SettingsClicked → ActivateSettingsAsync`.
- `SettingsWidgetPage` (`Type="Settings"`) enumerates all 8 slots with a
  `ComboBox` over the snapshot's `availablePanels`, a click-through toggle, and
  an opacity slider; Save validates that no panel is assigned twice and sends
  `kind="preferences"` (bounded JSON) through `SnapshotHub`; the hub's
  `AppServiceConnection.SendMessageAsync` is awaited by the widget and
  acknowledged only after LayerD atomically persists the preference sidecar.
- `WidgetSnapshotHub` is the in-process XAML snapshot bus: it validates and
  accepts snapshot JSON (rejecting mismatched or oversized documents), keeps one
  latest snapshot, fans out to all 9 widget CoreWindows under dispatch, and
  expires the latest after 2.0 s of silence. Each `PanelWidgetPage` subscribes
  on navigate and unsubscribes on leave; capability checks shrink the bus to a
  same-package connection (`current_process_is_expected_package()` exercises the
  fallback to "disconnected" in unpackaged/debug hosts).
- **Launch path (MSIX):** the UWP process's `FullTrustProcessLauncher`
  (`runFullTrust`) starts full-trust `layerd.exe` (which is single-instance via
  `Local\LayerC.LayerD.SingleInstance`) so the bridge's
  `publish_companion_snapshots` has a live peer without a preinstalled
  autostart. Private WinMD (`Microsoft.Gaming.XboxGameBar.Private.winmd`) is
  carried inside the package so the COM proxy stub and widget activation remain
  self-contained.

## 6. MSIX (`src/gamebar/Package.appxmanifest` + CMake)

- `Package.appxmanifest` declares an **x64** package and carries an **App
  Service** (`LayerC.GameBar.v1`) on the **UWP Application**, a
  **FullTrustProcess** (`layerd.exe`) for the autostart helper, **8 ×
  `windows.appExtension` (Standard)** plus **1 × `windows.appExtension`
  (Settings)**, and the **package-level `windows.activatableClass.proxyStub`**
  for the Game Bar private types (carrying the public and private WinMDs as
  in-package MBM payloads). The MSIX is built from a separate UWP project
  (`src/gamebar/widget/LayerC.GameBar.vcxproj`, C++/WinRT + XAML) via MSBuild
  (`/t:Restore` + `/t:Build` on the checked-in Nano Server target), then staged
  together with `layerd`/`layere`/`layercal`/`layerworker` and the x64 desktop
  VC runtime DLL set into the final `makeappx pack`. Missing redistributables
  disable the package target rather than producing an MSIX that fails on a
  clean machine. See `src/gamebar/StageGameBarPackage.ps1`.
- Widget registration uses the Xbox Game Bar SDK/host manifest extension,
  not the generic Windows 10 AppX schema.
- CI produces an unsigned test MSIX; official release packages are signed
  using an externally supplied certificate. Game Bar smoke coverage requires
  the full MSIX on a supported Windows image: run `lctest`, focus the fake
  game window, open Game Bar (Win+G), and inspect `LayerC Panel 1` and the
  sliders/persistence in `LayerC Settings` (the runtime diagnostics
  `Runtime Diagnostics… → gameBarTransport` shows `connected` and any
  `lastError`).

## Conventions summary

companion model -> validated sidecar -> bounded snapshot (+ catalog & preferences, stitch-checked) -> bridge
-> same-package App Service publisher/receiver -> 8+settings UWP widgets on an
in-process XAML bus with a 2.0 s staleness cap, settings edits that round trip
back to a validated bundle write.
