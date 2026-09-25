---
name: layere-ui-computer-use
description: Drive and visually inspect LayerC's native Windows editor, `layere.exe`.
---

# LayerE UI computer use

Drive the real LayerE build and verify the rendered/runtime result. Pair UI evidence with targeted source or test inspection when diagnosing a defect.

## Runtime and target

Use the bundled `computer-use` skill for native Windows control, startup, and
recovery mechanics. Browser-only state does not establish that native control is
unavailable; check the available native-computer tool before reporting a blocker.

Honor an executable path supplied by the user. Otherwise reuse one uniquely
identified running `layere.exe`, or select the newest relevant repository build
and report its absolute path, timestamp, and size. Identify the window by both
`layere.exe` and the exact `LayerC Editor` title. Do not infer binary provenance
from Git HEAD or a timestamp, and establish authenticated editor/daemon
connectivity before making live-integration claims.

## Observe and verify

Use the current state for each action: observe, select a labeled accessibility
element, act once, then observe the result. Reacquire the window after stale
handles or user interruption; indexes, coordinates, and handles are not durable.

Prefer accessibility controls for standard Win32 UI and current screenshot
coordinates for custom-rendered canvases or resizing. Treat accessibility state
and rendered pixels as separate evidence. If the capture shows another window or
stale pixels, reacquire LayerE and retry the observation; report a tool-limited
result if reliable state cannot be restored.

Dialogs may be embedded in the parent tree or exposed as separate windows.
Inspect fresh state before retrying a modal action, and do not repeat a mutation
whose result is unknown.

## Known navigation

Do not hard-code accessibility indexes; find controls by current labels.

- Advanced profile fields: select the `Advanced` checkbox.
- Automation editor: open `Tools`, then `Automation...`. The resulting `Automation` dialog exposes presets, rules/actions, selected-item fields, validation results, and Close.
- A validation row reading `No automation module` is an application state, not proof that the dialog failed to open.
- Visual placement: `Position on Window` and the preview are custom-rendered surfaces; verify with current screenshots as well as control state.

## Debugging workflow

- Reproduce the smallest user-described path in the current build and record the executable, selected profile, exact actions, and observed result.
- Avoid saving profiles, deleting items, or changing durable settings unless the user requested those mutations. Use disposable/test profiles for mutation-heavy reproduction when available.
- Before authorized changes, record the relevant baseline: selected/enabled profile, theme, window geometry, and edited values or modes. An unsaved draft is useful only after verifying that the tested action does not publish it. Track temporary changes and outstanding restoration; verify restoration through the UI and, for persisted edits, the document/runtime states. Preserve changes the user makes during an interruption instead of blindly restoring an older baseline.
- For a defect, identify the owning `src/editor/` flow and the narrowest relevant tests after reproducing it. Inspect daemon/runtime code only when evidence crosses that boundary.
- For autosave, history or live-overlay behavior, use [layere-draft-to-daemon-sync](../layere-draft-to-daemon-sync/SKILL.md) to distinguish editor, durable document and daemon results.
- Apply a fix only when requested. Rebuild the focused LayerE target, repeat the same UI path, and verify both accessibility state and rendered pixels where applicable.
- Do not automate terminals, authentication, Windows security/privacy settings, or the Codex/ChatGPT UI through Computer Use.

## Interface review

Review the real current build at representative window sizes and relevant modes. Check:

- control visibility, clipping, overlap, spacing, alignment, and text readability;
- focus order, keyboard operation, disabled/enabled states, default actions, and modal behavior;
- empty, populated, validation-error, unavailable-service, and reconnect states relevant to the requested surface;
- consistency between main editor, Automation, Appearance, Options, calibration, and other dialogs in scope;
- whether status text and failures are visible and actionable.

For a broad walkthrough, build a coverage table from the actual menus and reachable modes. Separate visiting a dialog from exercising its actions, and mark verified outcomes, findings, blocked paths and untested states. Do not turn a narrow defect reproduction into a full matrix.

Capture evidence for each actionable finding: build identity, reproduction steps, expected behavior, observed behavior, and screenshot/control-tree evidence. Separate reproduced defects, QoL suggestions, diagnostic hypotheses and tool limitations. Do not infer pixel-level defects from accessibility text alone or label an untested hardware/conditional path as passed.

During a long or interrupted review, save a checkpoint alongside the review evidence: tested builds, completed/pending coverage, last observed window/dialog, temporary mutations and remaining restoration. On resume, inspect live processes and windows before acting; saved handles, indices and an old dialog description are not current UI state. Treat prior reports as evidence examples, not permanent assumptions about product behavior.

When the user requests a durable LayerC review, write open findings to `review/layerN.review-YYYY-MM-DD.md` using the repository convention. Keep `review/` untracked and move a register to `review/archiv/` only after every finding is verified fixed. Leave unrelated worktree changes untouched.
