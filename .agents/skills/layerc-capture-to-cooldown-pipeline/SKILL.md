---
name: layerc-capture-to-cooldown-pipeline
description: Diagnose visual-rule failures across calibration, capture, evaluation, and cooldown activation.
---

# LayerC capture-to-cooldown pipeline

Trace only the boundaries implicated by the failing rule. Resolve commands and target names from the linked source and current CMake configuration.

## Establish the reproduction

- Record the build paths, target window/process, profile and cooldown IDs, observation type, trigger mode, capture quality, and whether isolated capture is enabled.
- Use a disposable profile and controlled target, preferably the existing `lctest` tool. Read [lctest](../../../tools/lctest.cpp) before selecting its mode; do not guess command-line switches.
- Use [layere-ui-computer-use](../layere-ui-computer-use/SKILL.md) when driving the editor or calibration dialogs. For Linux CLI calibration, use [Linux programs-per-environment verification](../layerc-linux-programs-per-environment/SKILL.md).

## Trace the first failing boundary

1. **Editor launch and merge:** inspect [editor_visual.cpp](../../../src/editor/editor_visual.cpp), especially `launch_calibration` and `merge_calibration_result`. Verify target identity, request/response validation, cancellation, observation IDs, and preservation of unrelated assignments.
2. **Calibration:** inspect [calibration_capture.cpp](../../../src/calibration/calibration_capture.cpp), [calibration_templates.cpp](../../../src/calibration/calibration_templates.cpp), and `CalibrationWindow::apply` in [calibration_window.cpp](../../../src/calibration/calibration_window.cpp). Check preview-to-normalized-region mapping, template structure/masks, and the returned validated visual JSON.
3. **Capture:** inspect [window_capture.cpp](../../../src/capture/window_capture.cpp) and the selected backend. Use [capture_probe.cpp](../../../tools/capture_probe.cpp) with its current usage to separate unavailable frames from bad evaluation. Record backend, availability, frame dimensions, and diagnostic reason.
4. **Evaluation and trigger:** follow [visual_capture_worker.cpp](../../../src/daemon/visual_capture_worker.cpp), [daemon_visual.cpp](../../../src/daemon/daemon_visual.cpp), and [visual_trigger_engine.cpp](../../../src/core/visual_trigger_engine.cpp). Compare the observation result, freshness, input-confirmation state, and resulting cooldown transition.

Windows calibration uses a live WGC preview but a frozen foreground snapshot for Desktop Duplication fallback, with a static-snapshot fallback path. Verify which path actually ran; a frozen preview is not itself a defect. Check foreground restoration when that path is involved.

Calibration Apply returns a response to the editor. It does not by itself prove that the merged profile was saved or consumed by LayerD. Verify that final boundary using [draft-to-daemon sync](../layere-draft-to-daemon-sync/SKILL.md).

When only isolated mode fails, use [demand-to-service lifecycle](../layerc-demand-to-service-lifecycle/SKILL.md) for LayerW startup, protocol, and recovery. Keep the same target/profile/quality when comparing in-process and isolated evaluation.

## Focused verification

- Math/region/quality changes: `layerc_capture_tests`.
- Pixel, template, trigger, or visual-profile semantics: relevant cases in `layerc_core_tests`.
- Calibration controls/resources: `layerc_editor_tests`, then the affected real dialog path.
- GPU integration: `layerc_capture_integration_tests`; check its CMake registration option and interactive desktop requirements first.
- Performance claims: select the visual workload through [commit-to-CI evidence](../layerc-commit-to-ci-evidence/SKILL.md); do not equate correctness tests with budget evidence.

Resolve actual targets and configuration from [CMakeLists.txt](../../../CMakeLists.txt) before building. After a fix, replay the smallest failing path through actual cooldown activation and one relevant unavailable/recovery transition.

## Completion

Verify the smallest failing path through cooldown activation and one relevant unavailable or recovery transition. Report build/target identity, backend, expected versus observed transition, focused checks, and any unverified hardware behavior; keep capture, matching, persistence, and daemon activation separate.
