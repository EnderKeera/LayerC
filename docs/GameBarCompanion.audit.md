# Game Bar Companion Audit

## Finding
The current Game Bar companion work is not implemented or build-integrated despite `docs/GameBarCompanion.md` claiming it is ready.

## Evidence
- `src/gamebar/` contains only `GameBarBridge.cpp`; the documented header, slot-mapping files, package manifest, and tests are absent.
- `GameBarBridge.cpp` includes `GameBarBridge.hpp` and `json.hpp`, but neither include is present at the documented path / target include setup.
- The bridge calls `nlohmann::json::parse`, while the repository's JSON implementation is `layerc::core::json` and no nlohmann dependency is configured.
- `HideStaleData()` is a placeholder and does not expire snapshots after two seconds.
- `Reconnect()` only calls the placeholder stale-data method; it does not reconnect.
- `ConnectAsync()` unconditionally returns `Success` without opening or authenticating an AppService connection.
- CMake has no Game Bar target or MSIX packaging step.

## Impact
The documented V2.4 status is materially inaccurate. A build or release claim should not be made until the code is implemented, integrated, and covered by the PLAN.md Game Bar gates.

## Immediate fix
Replace the status section in `docs/GameBarCompanion.md` with an audited incomplete status, then implement the bridge, sidecar, package, build integration, and tests in separate verified changes.
