# LayerC Game Bar Companion (v2.4)

## Overview

The Game Bar companion is an optional Xbox Game Bar extension that displays
LayerC cooldown panel snapshots in the 8 standard panel slots plus one settings
widget. It is **not** part of the portable ZIP; it ships only in the separate
MSIX package. This document describes what the current source tree actually
ships. For the authoritative release status, see `docs/GameBarCompanion.audit.md`;
for the package layout, see `src/gamebar/README.md`.

**Key invariants** (must never be violated):
- LayerD remains the sole source of truth for cooldown state.
- No capture, input processing, or mutation happens in the companion.
- The companion reconnects on LayerD/Game Bar/Explorer restart and hides stale
  data after 2 seconds.
- All measurements treat companion cost separately from native LayerD budgets.

## 1. Data model (`src/core/include/layerc/core/companion.hpp`)

- `CompanionProfile` — schema-1 profile sidecar: `profileId` + a bounded set
  of `CompanionSlotMapping` entries (0..7 slots) mapping a slot to a panel
  `panelId`, with `pinned`, `clickThrough`, and `opacity` overrides.
- `CompanionSessionSnapshot` — bounded immutable snapshot: `profileId`,
  `sessionKey`, `timestampMs`, and up to 8 panels, each with up to 32 item
  rows (cooldown id, name, icon, progress, charges, remaining time, ready,
  color). `progress` is the fraction of the cooldown elapsed toward ready
  (1.0 = ready, 0.0 = just triggered); `icon`/`color` and panel `visible`
  are reserved fields the bridge does not yet populate.
- All documents carry a 64 KiB bound and a 512-byte text bound.

## 2. Persistence (sidecar, `src/platform/profile_store.cpp`)

- Slot mappings persist per profile under `profiles/_companion/<profile>.json`
  (base schema 4, companion module schema 1) through the same crash-recoverable
  bundle transaction as base/visual/automation.
- Saving an unrelated base/visual/automation edit preserves an existing valid
  companion sidecar; an invalid existing sidecar fails the bundle closed.
- Writes are validated by `layerc::core::validate_companion_profile` (duplicate
  slot, duplicate panel, unknown panelId, out-of-range opacity).

## 3. Bridge (`src/gamebar/GameBarBridge.cpp`)

`layerc::gamebar::GameBarBridge` is a plain C++ snapshot stash: it formats an
immutable `CompanionSessionSnapshot` from session cooldown/panel state
(`create_session_snapshot`), keeps one latest snapshot under a mutex, and
hides data older than the 2.0 s cutoff (`latest_snapshot` /
`latest_snapshot_json`). It is not WinRT and owns no connection.

## 4. Transport (`src/gamebar/GameBarTransport.cpp`, daemon-owned)

- The daemon owns a current-user named pipe `\\.\pipe\LayerC.GameBar.v1`
  with a per-user SDDL ACL, `PIPE_REJECT_REMOTE_CLIENTS`, same-logon-session
  and MSIX package-identity (`LayerC.GameBar.v1`) client authentication, and
  overlapped I/O.
- `GameBarRuntimeService` starts/polls/stops the transport on demand; the
  daemon feeds it bounded, immutable framed JSON snapshots (64 KiB) when
  demands change and on the game-scan timer (`DaemonApp::publish_companion_snapshots`).
- Companion cost is accounted separately from the native-overlay budget.

## 5. Host (`src/gamebar/GameBarCompanionHost.cpp`, `layerc-gamebar.exe`)

- The host is the MSIX executable. Current state: a named-pipe one-way consumer
  that reads framed snapshots, bounds-checks them, and parses them; error exits
  2-5 are defined for pipe/size/schema/unreadable failures.
- **Outstanding (not implemented):** Windows AppService activation factory,
  Game Bar widget registration/rendering, and an end-to-end launch path (the
  daemon does not launch the host and Game Bar has no activation entry point yet).
  The host's `AppServiceConnection` construction is a package-readiness stub
  only. Watch `docs/GameBarCompanion.audit.md` for when this closes; do not
  describe the companion as a shipping Game Bar widget until it does.

## 6. MSIX skeleton (`src/gamebar/Package.appxmanifest` + CMake)

- `Package.appxmanifest` declares an App Service (`LayerC.GameBar.v1`) and
  the package identity; MSIX staging renames it to `AppxManifest.xml`. Widget
  registration uses the Xbox Game Bar SDK/host manifest extension, not the
  generic Windows 10 AppX schema.
- CMake builds `layerc-gamebar.exe` and, when `makeappx` is available,
  assembles an unsigned `LayerC.GameBar.v1.msix`. Official distribution still
  requires signing and Game Bar host smoke coverage on a supported Windows image.

## Conventions summary

companion model -> validated sidecar -> bounded snapshot -> stale-capped bridge
-> daemon-owned named-pipe publisher -> unsigned MSIX skeleton (activation
path outstanding).
