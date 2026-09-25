---
name: layerc-demand-to-service-lifecycle
description: Diagnose mismatches between capability demand and owned LayerD services, including LayerW lifecycle.
---

# LayerC demand-to-service lifecycle

Use the current contracts in [daemon_kernel.hpp](../../../src/runtime/include/layerc/runtime/daemon_kernel.hpp) and [runtime_services.hpp](../../../src/runtime/include/layerc/runtime/runtime_services.hpp) when lifecycle behavior is in scope.

## Follow demand to ownership

Trace `calculate_capability_demand` and `DaemonKernel::reconcile` in [daemon_kernel.cpp](../../../src/runtime/daemon_kernel.cpp), then the affected service descriptor and owned resources. Distinguish requested demand, platform availability, running state, and fault reason.

- Windows adapters: [daemon_services.cpp](../../../src/daemon/daemon_services.cpp), [daemon_sessions.cpp](../../../src/daemon/daemon_sessions.cpp), [daemon_input.cpp](../../../src/daemon/daemon_input.cpp), [daemon_visual.cpp](../../../src/daemon/daemon_visual.cpp).
- Linux adapters: [daemon_host.cpp](../../../src/daemon/linux/daemon_host.cpp), [input_service.cpp](../../../src/daemon/linux/input_service.cpp), [overlay_service.cpp](../../../src/daemon/linux/overlay_service.cpp), [visual_service.cpp](../../../src/daemon/linux/visual_service.cpp).
- Service reconciliation/rollback: [runtime_services.cpp](../../../src/runtime/runtime_services.cpp).

For the affected capability, collect observations before demand, during use, after a relevant fault/reload, and after the final consumer disappears. Use the same process identity and stable fixture. Compare owned handles/threads/capture sessions/cache with the starting baseline; do not require all process resources to become zero while unrelated services remain active.

Reproduce foreground loss, target destruction, shared-window consumers, or profile reload only when they affect the ownership transition under investigation. Check passive input and click-through through the existing input/overlay tests when touching those adapters.

## LayerW branch

Read [isolated_visual_worker.cpp](../../../src/daemon/isolated_visual_worker.cpp), [worker/main.cpp](../../../src/worker/main.cpp), and [isolated_visual_protocol.hpp](../../../src/daemon/isolated_visual_protocol.hpp). Obtain bounds, watchdog timing, and protocol versions from these sources.

1. Launch through a disposable LayerD session that demands isolated capture; LayerW is a daemon-owned helper, not a standalone capture CLI.
2. Trace sibling image, PID/user/session checks, nonce exchange, and Job assignment before accepting a ready state.
3. Verify configuration acknowledgement and snapshot acceptance independently. Duplicate snapshots can be legitimate; replay, stale results, or a quality-change epoch must follow the current sequencing policy.
4. For a requested recovery test, fault only the helper owned by the disposable session. Observe timeout/quarantine/retry behavior and whether unrelated cooldown/input services remain usable.
5. Remove demand or close the owned daemon and verify helper exit, joined threads, pipe/Job cleanup, and stable resource counts. Preserve a fault reason when capture is unavailable.

Do not launch a second production daemon or terminate an existing user's process just to obtain a clean baseline. Reuse an isolated test fixture or report the specific conflict.

## Focused verification

- Shared demand/reconciliation: `layerc_runtime_tests` and [runtime_services_tests.cpp](../../../tests/runtime_services_tests.cpp).
- LayerW serialization/authentication/sequencing: `layerc_isolated_visual_tests` and [isolated_visual_protocol_tests.cpp](../../../tests/isolated_visual_protocol_tests.cpp).
- Linux loop/control or X11 adapters: select the affected registered target through [Linux programs-per-environment verification](../layerc-linux-programs-per-environment/SKILL.md).
- Resource/latency claims: use relevant existing benchmarks via [commit-to-CI evidence](../layerc-commit-to-ci-evidence/SKILL.md).

The isolated protocol target does not launch the real helper. Report real process/Job cleanup and GPU behavior as unverified unless observed separately. Use [capture-to-cooldown pipeline](../layerc-capture-to-cooldown-pipeline/SKILL.md) when lifecycle is healthy but measurements or cooldown activation are wrong.

## Completion

Verify the relevant transition through final demand removal or visible unavailability. Report demand, fault/availability, owned resources, build/process identity, focused checks, and whether the final state recovered.
