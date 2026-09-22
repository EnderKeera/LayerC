# Game Bar Companion V2.4 — Release Evidence Record

This file tracks the **Game Bar companion only**. It is not the current status
of the whole LayerC project and does not claim that a signed MSIX has shipped.
The architecture contract lives in [`GameBarCompanion.md`](GameBarCompanion.md);
project-wide status lives in [`../README.md`](../README.md).

## Implementation baseline

The V2.4 host, activation path, App Service IPC, eight panel widgets, settings
widget, snapshot bus, package manifest, daemon transport, and companion-sidecar
persistence are implemented as described in `GameBarCompanion.md`.

## Recorded automated verification

The following is a historical evidence snapshot recorded on **2026-08-24**. It
was not rerun by later documentation edits:

- MSVC and MinGW Release suites: 10/10 at the time of recording.
- Unsigned `layerc_gamebar_msix` produced successfully.
- V2.2, V2.3, and latency gates passed.
- Recorded daemon metrics: 2.16 MiB private bytes, 11.5 MiB working set, and
  11.33 microseconds average evaluator time.
- CI used the `ci` preset with sccache and UNITY builds.

These values remain evidence for that run, not a substitute for current CI.

## Outstanding manual and certification evidence

A production Game Bar release remains incomplete until all of the following are
recorded:

1. **Reconnection matrix:** App Service recovery across LayerD, Game Bar,
   Explorer, and package restarts.
2. **Eight-slot concurrency:** all panel slots active concurrently with
   independent cooldowns.
3. **Settings persistence:** opacity, pinning, click-through, and slot mappings
   survive companion-sidecar and process restarts.
4. **Stale hiding:** every widget clears to disconnected after more than two
   seconds without a valid snapshot.
5. **Shell controls:** controller navigation, pinning, click-through, compact
   mode, transparency, and per-monitor DPI behavior.
6. **Resource accounting:** LayerC bridge/widget CPU, working set, and GPU memory
   measured separately from the Game Bar host.
7. **Fullscreen matrix:** exclusive-fullscreen and borderless-windowed behavior
   documented per graphics API.
8. **V1.1 visual resource gate:** resolve or formally disposition the recorded
   35.3–35.8 MiB private-byte result against the 35.0 MiB gate.
9. **Production signing/certification:** sign the final MSIX with the externally
   supplied certificate and record the applicable store/certification result.

## Completion boundary

Mark this record released only when the manual matrix, resource evidence, and
signing/certification artifacts are linked here or from an approved release
record. Compilation, an unsigned MSIX, and automated companion tests establish
narrower evidence only.
