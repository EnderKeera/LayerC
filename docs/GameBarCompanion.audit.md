# Game Bar Companion Audit & Release Status

## Current Status (V2.4 WIP)
Implemented per `docs/GameBarCompanion.md` §§1-5 (host, activation, IPC, widgets, bus, manifest, transport, persistence).

## Verification Completed (2026-08-24)

MSVC+MinGW Release 10/10, `layerc_gamebar_msix` unsigned MSIX, V2.2/V2.3/latency gates passed (2.16 MiB private / 11.5 MiB WS / 11.33 µs avg), CI `ci`+sccache+UNITY.

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
