# Layer 13 Review — V2.1 Template Recognition (First Pass)

Date: 2026-08-11 — Uncommitted V2.1 diff (`git diff` 33 files, 2196+ lines + 6 untracked). Automated review pass with full reads of all modified + untracked files.

## Overview

V1.1 `PixelRule`/`ruleId` generalized to typed `observations` (`pixel`/`template`), schema `1→2` migration, SHA-256 content-addressed template assets (`_visual/assets/<sha256>.png`), bounded CPU matcher (coarse + refinement), and editor/daemon/calibration plumbing. Added tests for template matching, asset GC, and calibration envelope. All paths relative to repo root.

---

## Bugs — requires action

### Medium

**1. Budget-exhaustion silently skips scale factors — false negatives under tight budget** `src/core/template_matcher.cpp:480`

```cpp
if (coarse_budget < reference.locator_samples.size()) break; // skips remaining scales
```

`reference_budget` is split across up to 4 refs per state (and 4 states). `coarse_budget` can drop below `locator_samples.size()` (3 at Low: 512 KiB /4 refs ≈128 KiB per ref; `coarse_budget` ≈48 KiB — still >3, but with tiny search regions, multiple states, or Exact fallback it hits the guard). Instead of increasing `step` to meet budget, the code `break`s and drops all remaining scale factors.

*Impact:* Missed match (false negative) only under tight budget / many references. Low with 4×4 templates is the realistic worst case.

*Fix:* Compute `step = ceil(sqrt(total_positions / coarse_budget))` clamped to guarantee ≥1 candidate per scale, rather than `break`.

### Low

**2. Capture vs import search region inconsistency** `src/calibration/calibration_window.cpp:868-925`

- `capture_template_reference()` expands search region to 2× capture (`expanded = min(1, extent*2)`) and centers it.
- `import_template_reference()` sets `search_region = rule_.region` (no expand) and `expected = rule_.region`.

Same user-drawn rectangle yields different `searchRegion`/`expectedSize` by source. Spec defines search region as user-selected normalized region; expanded behavior is undocumented.

*Fix:* Expand both or neither; document if expansion is intentional for capture.

**3. Non-current profile toggle can break atomic bundle invariant** `src/editor/editor_profiles.cpp:254-288`

Toggling `enabled` on a non-current profile reads `legacy_visual` and sets `commit_bundle = is_current || legacy_visual`. If base is v1 and sidecar is absent or already v2, `commit_bundle==false` → `write_profile_file()` writes base as v2 (via `serialize_profile_json` which always emits 2) while sidecar stays v1/absent. Next load still migrates, but "base and sidecar upgrade together" invariant (`PLAN.md:276`) is broken.

*Trigger:* User bulk-toggles a v1 base via list checkbox without opening it. Rare.

**4. `asset_signature` collision — stale decoded image until next quality change** `src/platform/visual_assets.cpp:337-346` / `src/daemon/daemon_visual.cpp:2142`

Daemon message thread uses `file_size ^ last_write_time` to detect repaired assets without decoding. Two different contents with same size and same `last_write_time` tick (NTFS 100 ns, but `MoveFileEx` may preserve time on rapid repair) yield same signature → `DaemonApp::reconcile_visual_captures()` skips `configure()` → worker keeps stale image until next quality change. Full SHA-256 verification is deferred to worker thread.

*Impact:* Bounded to one reconcile cycle; anomaly. Document or hash file on message thread if cheap, or note limitation.

### Info

**5. Sparse-mask Low promise exceeded — intentional** `src/core/visual.cpp:161`

`compact_template_image()` returns full-res image if compact would drop below 64 opaque pixels, then `prepare_template()` re-bounds it. A 512×512 sparse mask at Low retains full res for that reference, exceeding "Low retains ≤32 px axis" promise.

*Status:* Correct trade-off per comment; keep. No action.

---

## Structure / API

* `src/platform/profile_store.cpp:4` — `read_visual_file()` exposed from anonymous namespace to public header is correct for editor/tests; limit raised from 64 KiB to 256 KiB. Good.
* `src/editor/editor_schema.cpp:9` — `source_uses_schema_v1()` uses `version->as_number()==1.0` exact double compare. Valid for integer JSON `1` (exactly representable) but brittle if serializer ever emits floating error. Prefer integer field check or `llrint`. Not a bug in practice.
* `src/daemon/visual_capture_worker.hpp:384` — `VisualTemplateEvaluation::operator==` compares only `state` + `asset_signatures`, not `references`/`assets_available`. Intentional (worker-local cache), but change detection relies solely on signature — see Low #4.
* `src/core/visual.cpp:161` / `src/core/template_matcher.cpp:36-40` — matcher budgets and compact logic are correctly separated; comment explains sparse-mask trade-off.

---

## Performance — bounded

Matcher budgets `src/core/template_matcher.cpp:36-40`: Low 512 KiB, Balanced 1 MiB, Exact 8 MiB comparisons. Candidate caps `coarse_candidates=32`, `refinement_candidates=8`, `step = ceil(sqrt(total_positions / candidate_cap))` enforce bounded work. Verified.

`collect_active_asset_references` scans all profile files before GC `src/platform/visual_assets.cpp:96` — correctly fail-closed: invalid active sidecar aborts collection, while malformed unassociated base or corrupt stale `.bak` is ignored. Matches `tests/platform_live_tests.cpp:350` expectations.

No WARP/GDI fallback; Low/Balanced grids remain bounded.

---

## Behavior Changes — intentional, flag for awareness

* **Schema upgrade one-way with modal warning** `src/editor/editor_schema.cpp:18` / `src/editor/editor_window.hpp:251`. Timer autosave `src/editor/editor_dialog.cpp:418` `flush_autosave(false)` now pauses with status "Autosave paused — use Save to approve the schema-v2 upgrade" instead of prompting. Live-edit publishing also gated `src/editor/editor_live_edit.cpp:432`. Correct per `PLAN.md:273` but will surprise users with legacy profiles.
* **Limit increases:** `max_visual_json_bytes` 64 KiB → 256 KiB, `max_calibration_packet_bytes` 512 KiB → 2 MiB to hold two escaped 256 KiB docs (`tests/visual_profile_tests.cpp:653` uses 200 KiB of `\`). Deliberate.
* **Benchmark:** `tools/benchmark_v11.ps1` now requires template-triggered overlay visibility (`FindWindow("LayerDOverlayWindow")`) for `v21-template` workload. `pixel_evaluator` renamed to `ObservationMeasurement` with alias for compatibility — callers should migrate.

---

## Untracked files — must be added

`git status --short` shows 6 untracked files not yet `git add`ed:

```
?? src/core/template_matcher.hpp
?? src/core/template_matcher.cpp
?? src/platform/visual_assets.hpp
?? src/platform/visual_assets.cpp
?? src/editor/editor_schema.cpp
?? CLAUDE.md
?? layer1*.review-*.md  (this file and prior reviews)
```

CMakeLists already references the new sources — they will not build on a clean checkout until added. Run `git add` for all.

---

## Suggested next steps

1. Fix `coarse_budget` guard at `src/core/template_matcher.cpp:480` to increase `step` instead of `break`.
2. Unify `search_region` expansion in `src/calibration/calibration_window.cpp:868-925` (both paths or neither) and document.
3. Ensure `src/editor/editor_profiles.cpp:254-288` upgrades sidecar alongside base when serializing as v2, or defer base upgrade until profile is opened.
4. Document `asset_signature` collision window in `src/platform/visual_assets.cpp:337-346` or add full hash check on reconcile.
5. `git add` all new sources and review docs.

---

## Supplementary findings (second pass — not in first pass)

### Medium

**S1. Solid-color references can false-positive at any location** `src/core/template_matcher.cpp:207-210` (also `locator_score` at 312-314)

For a flat reference (`reference_variance <= 1e-9`) `structure_score` returns `1.0 - |Δmean|/255`, which does not depend on position. Consequences:

- The coarse locator scores **1.0 at every position** inside any region whose mean luma is within `color_tolerance` of the reference, so the "best 32 candidates" are arbitrary grid points and the hill-climb refinement cannot move them (its score is also 1.0 everywhere).
- Final confidence = `0.7 × structure + 0.3 × color_coverage`; for any region within `color_tolerance` of the reference color, coverage ≈ 1.0 too, so `confidence ≈ 1.0 ≥ threshold` **regardless of where the game element actually is**. A flat icon/button (or a template of a solid background color — exactly what `tools/benchmark_v11.ps1`'s seed-0 fixture uses) fires the trigger even when the element is absent or elsewhere. A different solid color with similar mean (within ~10-25/255) also passes the 0.90 default via the `|Δmean|/255` term.

*Fix:* reject near-flat references at capture/import (minimum luma variance gate on `prepare_template`), or require `color_coverage` to be position-specific (use the sampled color distribution at the candidate location, not a mean-based fallback).

**S2. Calibration runs the full matcher on the UI thread at capture rate** `src/calibration/calibration_window.cpp:1096-1126`

`update_match_status()` calls `template_matcher_.evaluate(...)` directly, and it runs on every WM_TIMER capture tick (33 ms) via `update_capture() → refresh_preview_frame()` (line 585-598). At `quality_ == exact` a 512×512 reference can cost up to 8M pixel comparisons per evaluation (`comparison_budget`) — 50-150+ ms of UI-thread work per tick while dragging the region or observing the match status; Low/Balanced are a few ms per tick. The dialog message loop blocks during evaluation.

*Fix:* pace template evaluation (every N ticks / ≤2-4 Hz), or evaluate on a worker thread and post the status text.

### Low

**S3. `candidates_evaluated == 0` conflates "outside output" with "matcher gave up"** `src/daemon/visual_capture_worker.cpp:278-280`

The worker maps `match.candidates_evaluated == 0` to `CaptureAvailability::outside_output`, and under the default `trigger_from_input` policy inputs pass through during `outside_output`. But 0 is also returned when the coarse stage never scored anything: `coarse_budget < locator_samples.size()` → `break` skipping all scales (`template_matcher.cpp:567`), empty prepared samples (line 508-510), or budget exhaustion before the first `++candidates_evaluated`. Only the "every candidate rejected by `candidate_is_available`" case is genuinely outside. With many references/states at Low (the same scenario as Medium #1 above), the passthrough decision is made on the wrong grounds.

*Fix:* return a distinct result flag for "search region outside available output" vs "no candidates evaluated", or have the worker re-check the region intersection itself.

**S4. On-window edit bypasses the schema-v2 upgrade gate** `src/editor/editor_live_edit.cpp:409-418`

`begin_edit_on_window` sends `begin_edit` with `profile_json = serialize_profile_json(profile_)` unconditionally — no `confirm_v2_upgrade()` guard, unlike the publish path (`send_live_profile`). A legacy v1 profile that is v2-serialized in memory pushes the unapproved v2 shape to LayerD's running session while disk stays v1 — inconsistent with "Do not publish an in-memory schema-v2 migration before the user has approved". Low severity (disk untouched, live session only), but the gate or a comment is missing.

**S5. Double prompt when leaving a legacy profile with pending changes** `src/editor/editor_profiles.cpp:54-61`

`confirm_unsaved_changes()` calls `flush_autosave()` (default `allow_upgrade_prompt = true`), which shows the modal v2-upgrade dialog (via `confirm_v2_upgrade`, `editor_live_edit.cpp:134`). Declining returns false, then the generic "The current profile has unsaved changes. Save them first?" dialog appears — and choosing Yes re-shows the upgrade prompt (through `save_profile` → `confirm_v2_upgrade`). Two stacked modal dialogs for one decision.

**S6. Calibration Apply can commit a template whose assets are unavailable** `src/calibration/calibration_window.cpp:1144-1156`

`apply()` guards `references.empty()` but not `template_images_.size() != references.size()` — even though `update_match_status()` (line 1110-1115) detects and displays "(one or more referenced template assets are unavailable)" and refuses to evaluate. If some references failed to load in `initialize()`, Apply still returns a valid (syntactically) profile; LayerD then permanently reports "template asset missing or corrupt" and the trigger is dead until re-capture.

*Fix:* block Apply when `template_images_.size() != template_state_.references.size()`.

---

## Verdict

No blocking bug on happy path. The `coarse_budget` skip (Medium #1) is the only logic error with realistic worst-case manifestation (4 template states ×4 refs at Low). Remaining items are low-probability edge cases, intentional trade-offs, or behavioral changes that are correct per plan but need documentation/UX notice.

**Second pass adds:** S1 (flat-template false positives — the most user-visible correctness risk, it breaks trigger semantics for solid-color references) and S2 (UI-thread matcher at exact quality — measurable freeze) are worth acting on before merge; S3-S6 are smaller but cheap.
