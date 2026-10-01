---
name: layere-draft-to-daemon-sync
description: Trace LayerE edits across the draft, durable profile, and active LayerD session.
---

# LayerE draft-to-daemon sync

Use [layere-ui-computer-use](../layere-ui-computer-use/SKILL.md) when native interaction or rendered evidence is required. This skill owns the document/runtime comparison.

## Establish three states

Using the UI skill's build identities and restoration checkpoint, record the selected profile/path and authenticated live-channel status. For each affected step distinguish:

1. The editor draft and history baseline.
2. The durable base profile and sidecars.
3. The profile revision/session currently used by LayerD.

For repeated saves and failure cases, capture the disposable profile's relevant base/sidecar values and history baseline before mutation. Check whether an action publishes immediately even when its dialog has a Close or Cancel button. Cancellation of a dialog is not proof that an earlier autosave or live publication was reverted. A status label alone cannot establish agreement between the three states.

## Follow the owning flow

Start with [editor_live_edit.cpp](../../../src/editor/editor_live_edit.cpp): `record_profile_change`, `flush_autosave`, `notify_profile_committed`, `handle_live_message`, and `apply_history_snapshot`. Read [editor_history.hpp](../../../src/editor/editor_history.hpp) or [editor_profiles.cpp](../../../src/editor/editor_profiles.cpp) (including schema-upgrade prompts) only for the involved transition.

On the daemon side, trace [daemon_live_edit.cpp](../../../src/daemon/daemon_live_edit.cpp) and [daemon_sessions.cpp](../../../src/daemon/daemon_sessions.cpp). Use [live_edit.cpp](../../../src/core/live_edit.cpp) and [live_channel.cpp](../../../src/platform/live_channel.cpp) when the evidence points to revision/protocol or authentication behavior.

## Exercise the relevant transitions

- **New draft versus existing document:** a new draft needs its initial Save; an existing document can schedule autosave. Verify the invalid-control and invalid-sidecar paths retain the last consistent durable document.
- **Legacy schema:** verify the explicit upgrade decision gates both durable publication and premature live publication. Cancellation should not be counted as a failed save implementation.
- **History:** check one text-edit group and one structural edit if applicable; undo/redo must restore the corresponding sidecars as well as the base profile. Observe how a successful commit rebases history.
- **Connected save:** follow the committed-profile notification and revision/profile key through LayerD's session update. Verify actual overlay or cooldown behavior appropriate to the edit.
- **Disconnected/reconnected save:** inspect the daemon start/reload fallback and live subscription restoration. Compare the main status with Runtime Diagnostics and the actual running daemon identity. Record an unavailable connection separately from a document defect; do not restart a working daemon merely to make an unrelated GUI review pass. Do not assume a successful disk write proves the running daemon has reloaded it.
- **Position on Window/direct drag:** check target selection, anchor publication, final commit, cancellation, and edit-session teardown when that mode is involved.
- **Calibration result:** verify merge preserves unrelated observations and reaches the same document commit gate; use [capture-to-cooldown pipeline](../layerc-capture-to-cooldown-pipeline/SKILL.md) for capture-specific behavior.

Avoid running every branch for a narrow defect. Stop at the first inconsistent boundary, fix it if requested, then repeat the reproduction and the nearest rollback/reconnect case.

## Focused verification

Use `layerc_core_tests` for live-message/history-independent semantic contracts and `layerc_platform_tests` for pipe/authentication or store boundaries. Use `layerc_editor_tests` for dialog resources, theme/schema helpers, and presets.

`layerc_editor_tests` links the live-edit code through `layerc_editor_ui` but does not exercise autosave or daemon synchronization, so passing it is insufficient evidence for autosave or daemon synchronization; build `layere` and any changed `layerd` code and repeat the real path. Use [profile store roundtrip](../layerc-profile-store-roundtrip/SKILL.md) for transaction failure/recovery experiments.

For LC Test or another passive-input fixture, distinguish tool-injected keys from physical Raw Input/HID evidence. A missing countdown after synthetic input does not isolate a runtime defect. Confirm the actual assigned chord and target/session before interpreting the result; mark hardware input unverified when that evidence is unavailable. Preview motion can establish an editor change; observe the real target overlay to establish live publication, and check durable values separately when persistence is in scope.

After temporary mutations, verify the relevant draft/base/sidecar values and live-edit session teardown against the checkpoint. An Undo that restores only the thumbnail does not establish disk or daemon rollback. Record any remaining mismatch instead of claiming complete restoration.

## Completion

Verify the first inconsistent transition and the nearest rollback or reconnect case. Report the minimal actions and draft/disk/daemon verdict, marking each boundary verified, inconsistent, or unobserved and keeping different editor/daemon builds distinct.
