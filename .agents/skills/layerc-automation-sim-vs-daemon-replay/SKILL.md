---
name: layerc-automation-sim-vs-daemon-replay
description: Reproduce automation mismatches between LayerE simulation and LayerD execution from event traces.
---

# LayerC automation replay: simulator vs daemon

Derive event schemas and limits from [automation.hpp](../../../src/core/include/layerc/core/automation.hpp) and the current parser. LayerC has no CLI replay contract; use the implemented trace and simulator paths.

## Build a faithful reproduction

Capture or construct the smallest base profile, automation module, initial persistent values, and event sequence that reproduces the issue. Preserve typed source fields, relative monotonic timestamps, origin tokens, and ordering while reducing unrelated events. Use disposable user-authored data for examples.

Read [automation_trace.cpp](../../../src/core/automation_trace.cpp) for `.layerctrace` import/export and [editor_automation_simulator.cpp](../../../src/editor/editor_automation_simulator.cpp) for live subscription, loading, stepping, and saving. Use [layere-ui-computer-use](../layere-ui-computer-use/SKILL.md) for UI execution. Respect the existing trace-export disclosure and inspect traces before including user-authored fields in shared evidence.

Validate the reduced graph and trace through existing core parsers before interpreting runtime results. Record the first divergent event, expected winning rules/effects, actual commit result, diagnostic, and resulting state.

## Separate simulation from host integration

- The current simulator reset constructs `AutomationRuntime` with empty persistent values. A trace alone therefore does not recreate a daemon session with saved state. Align the starting state in a focused test fixture or explicitly report the difference.
- Simulator Play changes UI pacing. Semantic timing comes from event timestamps and due timer processing, not the wall-clock interval between button actions.
- Inspect `step_simulator` for override expiration and due-timer ordering before comparing results with daemon dispatch.
- The simulator's evaluator result does not prove that host cooldown or overlay effects were applied. Trace shared validation/commit in [automation_session.cpp](../../../src/core/automation_session.cpp), Windows integration in [daemon_automation.cpp](../../../src/daemon/daemon_automation.cpp), and Linux integration in [input_service.cpp](../../../src/daemon/linux/input_service.cpp).
- Persistent checkpoints and restart behavior require a disposable store; use [profile store roundtrip](../layerc-profile-store-roundtrip/SKILL.md). Session creation/removal and reload issues belong with [demand-to-service lifecycle](../layerc-demand-to-service-lifecycle/SKILL.md).

## Focused verification

Choose the affected cases in [automation_tests.cpp](../../../tests/automation_tests.cpp), [automation_value_tests.cpp](../../../tests/automation_value_tests.cpp), [automation_contract_tests.cpp](../../../tests/automation_contract_tests.cpp), and [cooldown_input_tests.cpp](../../../tests/cooldown_input_tests.cpp), built through `layerc_core_tests`.

A useful regression fixture must assert observable state or effects at the failing event, including lack of partial external mutation when a winning effect is invalid. It should not merely round-trip the implementation's own output.

For a Windows/Linux integration difference, add or run the affected host/session test rather than assuming shared evaluator coverage reaches both adapters. For evaluator-cost changes, run `layerc_automation_benchmark`; consult [commit-to-CI evidence](../layerc-commit-to-ci-evidence/SKILL.md) for daemon benchmark evidence and current thresholds.

## Completion

Identify the first divergent event and verify the nearest affected evaluator or host path. Report fixture/trace provenance, starting-state assumptions, focused results, and any daemon behavior that remains unobserved; simulator-only results are evaluator evidence.
