# Game Bar Companion Audit & Release Status

## Current Status (V2.4 WIP)

The end-to-end Xbox Game Bar companion implementation is complete:
- **Host**: `src/gamebar/widget/LayerC.GameBar.vcxproj` builds a native C++/WinRT UWP XAML host (`layerc-gamebar.exe`).
- **Activation**: `App::OnActivated` routes `PanelSlot1`–`PanelSlot8` and `Settings` protocol activations to their XAML pages.
- **IPC**: `App::OnBackgroundActivated` hosts same-package `LayerC.GameBar.v1` App Service with LayerD as the authoritative client.
- **Widgets**: `PanelWidgetPage` renders cooldown timelines and charges; `SettingsWidgetPage` manages slot mappings and opacity/click-through preferences.
- **Snapshot Bus**: `WidgetSnapshotHub` validates bounded JSON, fans out to widget CoreWindows, and clears stale data after 2.0 seconds.
- **Manifest**: `Package.appxmanifest` registers 8 standard widgets, 1 settings widget, the App Service, full-trust LayerD launcher, and Game Bar private WinMD proxy stubs.
- **Transport**: `GameBarTransport` uses asynchronous App Service operations, reconnect backoff (100–2000 ms), and no-op zero-handle behavior when unpackaged.
- **Persistence**: Companion sidecars (`profiles/_companion/<id>.json`) round-trip through atomic multi-document profile transactions and `.layercprofile` packs.

---

## Verification Completed (2026-08-24 / 2026-08-25)

- MSVC 2026 Release build and full CTest suite passed (**0 warnings, 9/9 tests**).
- MinGW Release workflow passed (**0 warnings, 9/9 tests**).
- `layerc_gamebar_msix` target builds the UWP host and packages unsigned `LayerC.GameBar.v1.msix` with all 5 binaries, VC runtime, and Game Bar SDK metadata.
- Automated tests cover 8-slot snapshots, sidecar transactions, catalog/preference bounds, 2s stale expiry, and package identity.
- V2.2 modular runtime benchmark passed (2.16 MiB private, 11.5 MiB WS, 0% CPU baseline).
- V2.3 automation evaluator benchmark passed (11.33 us average, 1151 us maximum).
- Latency benchmark passed (-0.27 ms median, -1.29 ms p95 regression vs baseline).
- GitHub Actions CI workflow operational on `windows-2022` (`ci` preset with Ninja + MSVC dev environment).

---

## Remaining Release Gates

V2.4 remains **work in progress** until the following manual and certification gates are recorded:

1. **Reconnection Matrix**: verify App Service reconnects cleanly across LayerD, Game Bar, Explorer, and package restarts.
2. **8-Slot Simultaneous Test**: all 8 panel slots active concurrently with independent game cooldowns.
3. **Settings Persistence**: verify UI slider/toggle changes persist through the companion sidecar across restarts.
4. **Stale Hiding**: verify widget clears to disconnected state after >2s of transport silence.
5. **Shell Controls**: verify controller navigation, pinning, click-through, compact mode, transparency, and per-monitor DPI scaling.
6. **Resource Accounting**: measure LayerC bridge/widget vs Game Bar host CPU, working set, and GPU memory separately.
7. **Fullscreen Matrix**: document exclusive fullscreen vs borderless windowed compatibility per graphics API.
8. **V1.1 Visual Resource Gate**: resolve borderline private byte overshoot on visual recognition (35.3–35.8 MiB vs 35.0 MiB gate).
9. **Official Package Signing**: sign the production MSIX with the externally supplied certificate.
