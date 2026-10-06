---
name: layerc-linux-programs-per-environment
description: Verify LayerC Linux behavior in headless, X11, native, or Flatpak environments.
---

# LayerC Linux programs-per-environment verification

[docs/Linux.md](../../../docs/Linux.md) owns platform support, dependencies, command syntax, packaging permissions, and the physical evidence checklist. Use only the section for the environment under test.

## Identify the environment and target

Record commit/build provenance, native or Flatpak mode, desktop/session type, display/extensions, and configured XDG roots. Distinguish headless, Xvfb, physical X11, and unsupported graphical environments. A display variable alone does not establish physical-desktop evidence.

Use a disposable store/runtime directory for mutation tests. Inspect existing daemon/socket ownership before launching a daemon or issuing `layerctl stop`; ensure commands address the test instance. Do not change the user's login service or Flatpak permissions just to make a check pass.

## Choose the surface

| Surface | Workflow and source |
| --- | --- |
| Headless control | Verify status/reload/stop, stdout payload versus stderr diagnostics, socket ownership, stale-socket behavior, bounded framing, and peer authentication in [control_channel.cpp](../../../src/platform/linux/control_channel.cpp) and [daemon_host.cpp](../../../src/daemon/linux/daemon_host.cpp). |
| Profile/bundle commands | Read current usage in [layerctl.cpp](../../../src/ctl/layerctl.cpp); use [profile store roundtrip](../layerc-profile-store-roundtrip/SKILL.md) for transaction and transfer cases. |
| Window/input/overlay | Select a controlled target; verify executable/title/class matching, focus and hierarchy changes, passive input, visible panel geometry, and actual pointer click-through. Inspect adapters under `src/platform/linux/` and `src/daemon/linux/`. |
| Capture/calibration | Read [linux_main.cpp](../../../src/calibration/linux_main.cpp) and [x11_capture.cpp](../../../src/platform/linux/x11_capture.cpp). Verify snapshot metadata, region bounds, PPM header/payload size, backend/fallback diagnostics, and resource cleanup. Keep binary PPM output separate from text. |
| Service/resource behavior | Use [demand-to-service lifecycle](../layerc-demand-to-service-lifecycle/SKILL.md) to compare demand, faults, and resources across session creation and teardown. |

Linux `layercal` is a snapshot CLI; do not route it through the Windows calibration dialog/request protocol. Visual capture success still needs a daemon rule transition when the requested claim is end-to-end behavior.

## Automated checks

Resolve available targets from [CMakeLists.txt](../../../CMakeLists.txt) and the test registrations in [cmake/LayerCTests.cmake](../../../cmake/LayerCTests.cmake). Select the corresponding `layerc_linux_*_tests` target for store, pack, loop, control, X11 tracking, input, overlay, capture, or visual integration; use `ctest -N` to confirm registration before a targeted run.

Read [ci-linux.sh](../../../tools/ci-linux.sh) when reproducing container CI. Its full container gate is appropriate for a requested CI reproduction, not the default conclusion to every Linux change. Report skips as unavailable coverage; required CI rejects skipped registered tests.

## Physical native/Flatpak evidence

Use [run-linux-v26-evidence.sh](../../../tools/run-linux-v26-evidence.sh) only on the selected physical X11 desktop after reading its current usage and cleanup behavior. It launches a daemon and sends control commands, so establish test-instance ownership first.

Supply the observed target XID and actual desktop/mode. The script's manual checklist is part of the evidence: do not prefill `LAYERC_MANUAL_RESULT=pass` merely because commands succeeded. Record pass only from observed checklist results; otherwise preserve the incomplete/failed result and reason. A `-` target omits calibration capture and cannot support a capture verdict.

Follow the native/Flatpak and desktop combinations currently required by the owning document. Keep automated Xvfb results separate from physical input, compositor, scaling, capture, and sandbox observations.

## Completion

Verify the requested surface in the identified environment and report build identity, fixture roots/target, command results, skips, artifacts, and manual observations. Update release evidence only for combinations actually observed.
