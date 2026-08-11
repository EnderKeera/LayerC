# Layer 11 Review — Visual Trigger / Calibration Patch (Second Pass)

Date: 2026-08-10 — Uncommitted changes (`git diff` + `git diff --cached` + `git status --short`: 33 modified, 15+ untracked, 1263 insertions / 84 deletions). Full reads of all modified + untracked files. Compared to `layer11.review.md` (2026-08-09).

Previous review items marked **FIXED** or **REMAINING** below. All paths relative to repo root.

## Verified fixes vs 2026-08-09 review

1. **FIXED — Inconsistent profile/visual write order** `src/editor/editor_live_edit.cpp:115` / `src/editor/editor_profiles.cpp:291`
   - Both paths now unified to `write_profile_bundle()` in `src/editor/editor_live_edit.cpp:122` `flush_autosave()` and `src/editor/editor_profiles.cpp:328` `save_profile()`.
   - `src/platform/profile_store.cpp:194` `write_profile_bundle()` implements ordered replace + rollback (visual-first if valid backup exists, otherwise profile-first) preserving `profile_id` consistency vs `src/core/visual.cpp:404` `validate_visual_profile()` and `src/daemon/daemon_sessions.cpp:133` `blocks_all_triggers()`. Correctly avoids `VisualSidecarState::invalid` blocking all triggers.

2. **FIXED — Fallback foreground steal** `src/calibration/calibration_window.cpp:483` / `:500`
   - `previous_foreground_` now restored via `SetForegroundWindow(restore)` with `SW_SHOW`/`SW_SHOWNOACTIVATE` at `src/calibration/calibration_window.cpp:486` / `:498`. No longer discarded via `previous_foreground_ = nullptr`.

3. **FIXED — Apply path canonical vs disk path** `src/daemon/daemon_sessions.cpp:287`
   - `src/daemon/daemon_sessions.cpp:83` `profile_path_for_key()` added; `src/daemon/daemon_sessions.cpp:312` `apply_profile_update()` now resolves real `entry.path()` before `load_visual_sidecar()`. Only fallback for new live-edit keys uses canonical `\\?\` path (returns `absent` on miss — safe).

4. **FIXED — Calibration hang (crash case)** `src/editor/editor_visual.cpp:256` → `:410`
   - Now has reader thread + `CreateEventW`, 30 min job timeout, `CancelSynchronousIo` + `WaitForSingleObject(reader_finished,2000)` before `join()`. Crash → `ReadFile` returns `ERROR_BROKEN_PIPE` and unblocks. Prior permanent `reader.join()` block on crash resolved.

5. **FIXED — Exact on UI thread (input latency)** `src/daemon/daemon_visual.cpp:96` → `src/daemon/visual_capture_worker.cpp:121`
   - `evaluate_pixel_rule()` moved to `src/daemon/visual_capture_worker.cpp:121` worker thread; `src/daemon/daemon_visual.cpp:55` `poll_visual_captures()` only does cheap `on_measurements()` debounce. Input latency fixed. Low/Balanced bounded via `src/capture/window_capture.cpp:159` `capture_sample_dimensions()` aspect-preserving 64/256.

6. **FIXED — Daemon status stomp** `src/editor/editor_live_edit.cpp:218` → `:214`
   - Now `handle_live_message(runtime_status)` only sets `visual_runtime_status_` + `refresh_visual_controls()` (`IDC_VISUAL_RUNTIME_STATUS`), not main `set_status()`. Transient editor messages no longer overwritten.

---

## Bugs — requires action

### Medium

**1. Blocking `WriteFile` on UI thread in `src/editor/editor_visual.cpp:416`**

```cpp
bool wrote = write_framed_pipe(request_write, encode(...), max, err);
```

`write_all()` in `src/platform/framed_pipe.cpp:18` loops `WriteFile` blocking until child drains. Payload ~130 KiB > anon pipe buffer (~4 KiB). If `layercal.exe` hangs before `read_framed_pipe()` (D3D deadlock, `capture_->poll()`), editor UI is frozen *before* the modal `MsgWaitForMultipleObjects` loop starts. Severity depends on Exact or driver hang; reproduces with a hung calibration child.

Fix: do the write on the reader thread or use overlapped I/O with timeout (e.g., move `write_framed_pipe` into the thread that already owns `response_read`, or `WriteFile` with `OVERLAPPED` + `WaitForSingleObject`).

**2. `write_profile_bundle()` stash uses move, not copy — crash between stash and commit loses data `src/platform/profile_store.cpp:211`**

Stash does `MoveFileExW(original,temp, WRITE_THROUGH)` for `sidecar` + `.bak` when `visual_profile==nullopt` (removing visual). Crash after move but before `write_profile_file()` succeeds leaves both originals in temp files (`pid.seq.tmp`) and original paths absent. On reboot `load_visual_sidecar()` returns `absent` (data lost, but intended delete). For the adding-visual path the opposite order is protected: visual-first with `protect_previous` or profile-first with restore lambda. The move-based remove path has no restore on crash.

Low probability but not crash-safe; use `CopyFileW` + delete on success (or keep `MoveFileExW` but restore on failure before returning error).

**3. Shared-window quality promotion forces Low to pay Exact cost `src/daemon/daemon_visual.cpp:27` / `src/daemon/visual_capture_worker.cpp:27`**

`configure()` takes `max(rate)` and `max(quality)` for all sessions on same `HWND`. Two profiles on same game window (e.g., `low` + `exact`) upscales the `low` session to `exact` pixel reads (up to ~8M pixels × 15 Hz). Document or split workers per quality (one worker per `(HWND, quality)` bucket) — otherwise V1.1 35 MiB / 3% CPU budget is bypassed. If shared-window multi-profile is unsupported, assert/document single-quality constraint.

### Low

**4. `rebase_visual()` mutates entire undo/redo stacks `src/editor/editor_history.hpp:73`**

```cpp
for(auto &s: undo_) s.visual_profile = visual_profile;
```

Visual calibrations are committed immediately via `save_visual_profile() -> rebase_visual()` and not undoable. This silently rewrites history: an undone base edit later redone will carry the *new* visual, losing the visual that was current at that undo point. Intended per comment but makes redo non-deterministic. Consider preserving per-snapshot visual or making visual changes explicit undo entries (so `undo` restores prior visual).

**5. `region_is_available()` epsilon asymmetric `src/capture/window_capture.cpp:145`**

`x+eps >= avail.x` but `x+width <= avail.x+width+eps`. A rule exactly at `avail.x - 0.5*eps` is incorrectly `outside_output` on stale WGC/Duplication frames. Use symmetric `±eps` check: `x+eps >= avail.x+eps` / `x+width-eps <= avail.x+width+eps` or expand avail by `eps` on both sides.

---

## Structure / nits

* `src/daemon/daemon_sessions.cpp:250` duplicated `VisualSidecarState` → `VisualSourceHealth` `switch` (also `:388`). Extract `toHealth()`.
* `src/platform/profile_store.cpp:132` `replace_file_atomically(path,contents,protect,max)` now takes `max_bytes` — good fix for prior 64 KiB profile vs 64 KiB visual confusion.
* `src/core/visual_trigger_engine.cpp:215` `measurement_is_fresh = 3000/rate` (200/300/600 ms) is intentional hardening; with `stable_samples=15` the *first* fresh input at 15 Hz still arms `pending_confirmation` for 66 ms. Document.
* `src/core/pixel_evaluator.cpp:66` `limit = 64/256/max` sampled as `min(region_width,limit)` per dimension (up to 4096/65536 samples) — different from `capture_sample_dimensions()` aspect-preserving readback at `src/capture/window_capture.cpp:159`. Works but the two limits should be named distinctly (`evaluation_samples_bound` vs `capture_dimensions_bound`).

## Performance — acceptable with note

* Exact still ~8M pixel samples per rule when used, now correctly off UI thread. Shared-window `max(quality)` promotion (bug 3) is the remaining budget risk.

## Security

No blocking security/injection issues found; live channel remains framed + bounded (`max_visual_json_bytes` / `max_calibration_packet_bytes`).

## Untracked files reviewed (full reads)

`src/core/visual.cpp`, `src/core/pixel_evaluator.cpp`, `src/core/visual_trigger_engine.cpp`, `src/core/calibration.cpp`, `src/platform/framed_pipe.cpp`, `src/capture/*.cpp`, `src/daemon/visual_capture_worker.*`, `src/daemon/daemon_visual.cpp`, `src/editor/editor_visual.cpp`, `src/calibration/*`, `tests/visual_profile_tests.cpp`, `tests/pixel_engine_tests.cpp`, `tests/capture_math_tests.cpp`, `tools/capture_probe.cpp`, `tools/benchmark_v11.ps1` — all consistent; no additional blocking bugs beyond items above.

## Suggested next steps

1. Move `write_framed_pipe` off UI thread or to overlapped I/O with timeout (`src/editor/editor_visual.cpp:416`, `src/platform/framed_pipe.cpp:18`).
2. Change stash for `visual==nullopt` path to copy-then-delete (`src/platform/profile_store.cpp:211`).
3. Decide on shared-window policy: document single-quality-per-HWND or split worker per quality (`src/daemon/daemon_visual.cpp:27`).
4. Fix epsilon symmetry (`src/capture/window_capture.cpp:145`) and optionally extract `toHealth()` helper.

---

## Verification 2026-08-10 — Was `layer11.review.md` (2026-08-09) fixed?

Re-checked all 8 findings from `layer11.review.md` (High 1, Medium 2-4, Second-pass A-D) against current working tree (full file reads + `git diff`).

| # | Original finding | Location | Status | Evidence |
|---|---|---|---|---|
| **1 High** | Inconsistent visual/profile write order → `profileId` mismatch → `VisualSidecarState::invalid` blocks all triggers | `src/editor/editor_live_edit.cpp:115` vs `src/editor/editor_profiles.cpp:291` | **FIXED** | Both paths now call `write_profile_bundle()` at `src/editor/editor_live_edit.cpp:122` `flush_autosave()` and `src/editor/editor_profiles.cpp:328` `save_profile()` (also `save_profile_enabled:189`). `src/platform/profile_store.cpp:194` `write_profile_bundle()` is transactional: visual-first with `protect_previous` if `previous_primary_pair_valid\|\|previous_backup_pair_valid`, else profile-first, with restore lambdas `:289` and `:310`. `profile_id` synced before call `:122` `if(visual_profile_) visual_profile_->profile_id=profile_.id`. Prevents `src/core/visual.cpp:404` `validate_visual_profile` `invalid` path. Caveat: removing-visual branch uses `MoveFileExW` stash `:211` — crash between stash and commit still loses file (new Medium #2), low probability. |
| **2 Med** | `begin_fallback_snapshot` steals foreground, discards `previous_foreground_` | `src/calibration/calibration_window.cpp:483`/`:500` | **FIXED** | `begin_fallback_snapshot:489` saves `GetForegroundWindow()`; `finish_fallback_snapshot:501-506` now computes `restore = IsWindow(previous_foreground_) ? previous_foreground_ : dialog_`, does `ShowWindow(dialog_, restore==dialog_?SW_SHOW:SW_SHOWNOACTIVATE)` + `SetForegroundWindow(restore)` at `:505-506`, then `previous_foreground_=nullptr`. No longer `if(prev!=target) prev=nullptr; SetForegroundWindow(dialog_)`. |
| **3 Med** | `apply_profile_update` builds sidecar path from canonical `\\?\` key | `src/daemon/daemon_sessions.cpp:287` | **FIXED** | Added helper `profile_path_for_key:83` that scans `profile_directory()` and matches `canonical_profile_key(entry.path())==key` to return real `entry.path()`. `apply_profile_update:316` now `source_path = found==end ? profile_path_for_key(key) : found->path; if(empty) source_path=path(key)`. I/O uses real disk path; fallback `\\?\` only for not-yet-on-disk live-edit key → `load_visual_sidecar` returns `absent` safely. |
| **4 Med** | `launch_calibration` blocks indefinitely on crash / hang | `src/editor/editor_visual.cpp:256` | **PARTIALLY FIXED** | Crash path fixed: reader thread + `CreateEventW` at `:394`, `read_framed_pipe` in thread `:410-413` + `SetEvent(reader_finished)`, `WaitForSingleObject(reader_finished,2000)` + `CancelSynchronousIo` + join `:449-455`, `ReadFile` returns `ERROR_BROKEN_PIPE` per `src/platform/framed_pipe.cpp:36`. Hang path bounded: job object `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` at `:375-383`, 30-min `calibration_timeout_ms=30*60*1000` loop at `:422-444` with `TerminateJobObject`/`TerminateProcess` + `WaitForSingleObject(process,2000)`. Also `attribute_size` check now correct `:311-324` (`probed!=FALSE || GetLastError()!=ERROR_INSUFFICIENT_BUFFER || size==0`). **Remaining**: `write_framed_pipe` at `:416` still synchronously on UI thread before `EnableWindow(FALSE)` modal loop — ~130 KiB > 4 KiB pipe buffer blocks until child drains; hung `layercal.exe` before `read_framed_pipe` freezes editor pre-loop. This is new Medium #1 and was not present as distinct item in original review. |
| **A Med** | `exact` stalls daemon UI thread (input latency) | `src/daemon/daemon_visual.cpp:96` | **FIXED** | `evaluate_pixel_rule` moved off `WM_TIMER` thread to `src/daemon/visual_capture_worker.cpp:121` inside `run()` worker. `src/daemon/daemon_visual.cpp:55` `poll_visual_captures()` now only does `on_measurements()` debounce at `:84`. `capture_sample_dimensions:159` caps Low/Balanced; Exact still full-res but on worker. |
| **B Low** | Daemon status stomps editor status bar | `src/editor/editor_live_edit.cpp:218` | **FIXED** | `handle_live_message:214-219` now early-returns for `LiveMessageType::runtime_status` with `visual_runtime_status_=to_wide(text); refresh_visual_controls(); return;` — dedicated `IDC_VISUAL_RUNTIME_STATUS` only, not `set_status()`. |
| **C Low** | `calibrate_selected_task` hides dialog before early return | `src/editor/editor_visual.cpp:236-253` | **FIXED** | Now at `src/editor/editor_visual.cpp:250-256` does `ShowWindow(task_dialog_,SW_HIDE)` then `if(!launch_calibration(...)){ ShowWindow(task_dialog_,SW_SHOWNORMAL); SetForegroundWindow(...); set_status(...); }`. `launch_calibration` returns `false` for notice decline, missing `layercal.exe`, pipe/attribute failures — all restore dialog. |
| **D Low** | Permanent freeze if calibration child *hangs* (not crashes) | `src/editor/editor_visual.cpp:256` ext | **PARTIALLY FIXED** | Same mechanism as #4: no longer permanent — bounded by 30-min job timeout `:422-444`. Still poor UX (30 min perceived hang) and blocked pre-loop write (`:416`) means timeout loop never starts if child hangs before read. Overall hang risk reduced but not eliminated. |

**Other items in original review:**

* Correctness note `daemon_input.cpp:177` `blocks_all_triggers()` — still intentional global block (`daemon_app.hpp:83`); unchanged, flagged as confirm-intent in both reviews.
* `visual_trigger_engine.cpp:215` `measurement_is_fresh = 3000/rate` (200/300/600 ms) — unchanged at `:218`, still intentional hardening (with `stable_samples=15` still arms `pending_confirmation` for 66 ms at 15 Hz).
* `daemon_visual.cpp:76` throttling + `reconcile_visual_captures:27` `max(rate)` — unchanged.
* Structure `daemon_sessions.cpp:258` duplicate switch — still single switch at `:255-272` found (no second copy at `:388` in current tree), but no `toHealth()` helper extracted yet.
* `profile_store.cpp:132` `replace_file_atomically` now `max_bytes` param — **FIXED** at `:132-135`.
* Performance `pixel_evaluator.cpp:66` Exact bypasses budget — now off UI thread but budget still bypassed (documented via `calibration_window.cpp:685` warning); remaining risk tracked as new Medium #3 (shared-window `max(quality)` promotion).

**Conclusion:** 6 of 8 original findings fully fixed, 2 partially fixed (Medium #4 / D — blocking `WriteFile` on UI thread remains, bounded only after write). All new Medium/Low bugs from `layer11.review-2026-08-10.md` (stash `MoveFileExW`, shared-window `max(quality)`, `rebase_visual` history rewrite `src/editor/editor_history.hpp:73`, `region_is_available` epsilon `src/capture/window_capture.cpp:145`) are **still open** and were not in original review.

---

## Additional findings — automated review pass (2026-08-10, same working tree)

Not recorded in the passes above; line references re-verified against the current tree. Continuation numbering 6-8; items 1-5 above are unaffected.

### Medium

**6. `triggerFromInput` not honored during capture `starting` window — input silently dropped** `src/core/visual_trigger_engine.cpp:64-71` / `:83-90`

In pixel-only replace mode (`keep_input=false`), the fallback request is only emitted when the rule `has_sample` **and** availability is terminal (`unavailable`/`protected_or_black`/`outside_output`). During `starting` (and `inactive`) — the first ~0.1–2 s after the worker spawns while WGC/DD delivers its first frame — `has_sample` is false, so a configured `trigger_from_input` policy is not honored and the key press is dropped with no trigger.

Contradicts the README claim ("both modes can explicitly choose triggerFromInput when capture is unavailable") for exactly the window users hit first: the moment the game becomes foreground. Only affects the brief startup period. Fix: treat `starting`/`inactive` under `trigger_from_input` as fallback too, or defer input handling until the first frame arrives.

### Performance

**7. Desktop Duplication fallback copies the entire desktop every frame** `src/capture/d3d_capture_support.cpp:139`

`CopyResource(staging_, texture)` copies the full desktop surface on every poll, even though only the window's intersection box (`source`) is consumed and readback is bounded to 64/256 px (`capture_sample_dimensions`). On a 4K desktop at 15 Hz that is a ~33 MB staging surface and ~500 MB/s GPU→CPU traffic per active worker — disproportionate to the "read back only the small result needed" claim, which holds only for the WGC path (texture is the window itself). `CopySubresourceRegion` with the intersection `D3D11_BOX` would bound the transfer to the window rect. Only affects the degraded DD fallback path, which the UI already flags.

### Low

**8. Calibration DD-fallback snapshot never succeeds on static desktops** `src/calibration/calibration_window.cpp:446-458`

`update_capture()` relies on `capture_->poll(now, true)` delivering a frame within the 2 s `fallback_deadline_` while the target window is forced foreground. If DD's `AcquireNextFrame` keeps returning `DXGI_ERROR_WAIT_TIMEOUT` (static scene, nothing repainting), no frame arrives and the preview terminates with "could not capture the target" — users on static desktops may never get a usable snapshot via the fallback path. Minor UX issue, not a correctness bug.

### Info / nits

- `src/core/visual_trigger_engine.cpp:92-94` — every unconverted key press in `confirm_input` mode re-arms `confirmation_deadline = now + timeout`. Mashing the key keeps the confirmation window open indefinitely; bounded per-press only if the retry semantics are intentional. Flagging in case it is unintentional.
- `src/core/pixel_evaluator.cpp:107-110` / `:123-132` — `average_color` rounds each channel via `std::lround`, but the match predicate compares the raw average against `target ± tolerance`; a target within tolerance of the true average can miss by up to 1.0 after rounding. Negligible in practice.
- `src/daemon/visual_capture_worker.cpp:145-152` — `latest_.evaluations` publication is gated on `applied_request_revision == request_revision_`; after a reconfigure the daemon's `poll_visual_captures` sees a snapshot with no evaluations for the session. The sequence-skip logic handles this gracefully; no action.
- `src/editor/editor_visual.cpp:422-444` — 30-minute `calibration_timeout_ms` is generous; a hung (not crashed) `layercal.exe` leaves the editor disabled for the full wait. Consider a shorter safety limit (5–10 min) if acceptable for the slowest capture path. This sharpens the hang-UX concern already recorded under Medium #1 / D, not a new issue.

---

## Additional findings — second automated review pass (2026-08-10, same working tree)

Same working tree as the passes above; all 39 new/modified TUs compiled clean at `/W4 /permissive-`, and `layerc_core_tests`, `layerc_capture_tests`, `layerc_platform_tests` all pass. Hand-rolled WGC ABI IIDs, vtable order, and `pixel_format_bgra8_unorm = 87` verified against Windows SDK metadata. Line references re-verified against the current tree. Continuation numbering 9-12; items 1-8 above are unaffected. No high-severity bugs found.

### Low-Moderate

**9. Worker wakes at 30 Hz regardless of the configured sample rate** `src/daemon/daemon_visual.cpp:105` / `src/daemon/visual_capture_worker.cpp:50`

`src/daemon/daemon_visual.cpp:105` calls `worker->set_excluded_screen_rects(exclusions)` on every `visual_capture_timer` tick (~33 ms). `VisualCaptureWorker::set_excluded_screen_rects` (`src/daemon/visual_capture_worker.cpp:50`) always reassigns the vector, bumps `exclusion_revision_`, and `wake_.notify_all()` — the revision never checks whether the set actually changed. The worker's `wait_for` sleep (1000/rate, up to 200 ms) is therefore interrupted every 33 ms, so:

- The backend is polled at ~30 Hz instead of the profile's 5/10/15 Hz cadence (extra `AcquireNextFrame(0)`/`TryGetNextFrame` calls).
- `black_frames_` accumulates at 30 Hz, so `protected_or_black` (30 counts) is reached in ~1 s of black — one third of the 3 s implied by a 15 Hz sample rate and well under what a 5 Hz profile implies. The README documents the freshness window as "three sample intervals (200 ms at 15 Hz, 600 ms at 5 Hz)"; the black-frame timing silently diverges from that contract.

Fix: compare the exclusion set before bumping the revision (or track a revision in the daemon), and only notify on actual change. The WGC backend ignores exclusions entirely, so this also currently forces a useless wake + backend poll per tick for the primary backend.

### Low

**10. `protected_or_black` false positives for legitimately dark frames** `src/capture/wgc_backend.cpp:98` (duplicated at `src/capture/desktop_duplication_backend.cpp:36`)

`frame_is_nearly_black` declares a frame black when fewer than 0.5% of sampled pixels exceed RGB 3. A game HUD or scene that is genuinely that dark (e.g., a loading screen, a dark menu) will hold `protected_or_black` for 30 polls (~1 s given issue 9), which the trigger engine treats as terminal — pixel-only triggers and input confirmation both go dead until a brighter frame arrives. Combined with issue 9's faster counting, a brief dark transition can suppress triggers for a noticeable window. Not a bug in the strict sense (black is deliberately treated as unavailable), but the counting cadence makes the recovery slower than the documentation implies.

**11. `launch_calibration` returns `true` on its handled-error paths** `src/editor/editor_visual.cpp:429`

`src/editor/editor_visual.cpp:429` (and the other MessageBox error paths) return `true` after showing the error dialog. Callers use the boolean to decide whether the launch failed: `calibrate_selected_task` (`src/editor/editor_visual.cpp:271`) only restores the hidden task picker and reports "Visual calibration was not started." on `false`. So on these paths the picker dialog stays hidden with no status feedback; the user has to reopen the task window manually. Consider returning `false` on failure paths (or restoring the dialog unconditionally before returning `true`).

**12. Stale `.bak` survives a repair write** `src/platform/profile_store.cpp:211`

When the primary sidecar is corrupt and calibration repairs it, `write_visual_sidecar` (`src/platform/profile_store.cpp:211`) sets `protect_previous = false` because the corrupt primary fails to parse — the old `.bak` is left untouched. If the new primary is later corrupted again, `load_visual_sidecar` recovers that stale backup, which references the pre-repair rule/trigger set; if the base profile has since changed (cooldowns removed/renamed), the backup fails validation and the profile enters the blocking-invalid state instead of simply falling back. Conditions: corrupt primary → repair via Calibrate → primary corrupted again later. Suggest updating or deleting the stale backup when writing over an invalid primary.

### Notes (verified, no action needed)

- Bundle write ordering/rollback (`write_profile_bundle`), the trigger state machine, and the calibration child-process isolation are all sound; new bundle rollback and framed-pipe tests pass.
- `process_input`'s fail-safe on invalid sidecars (`blocks_all_triggers`), `triggerFromInput` semantics, and the freshness window all match the documented behavior.
- `poll_visual_captures` also re-reads `worker->snapshot()` and re-pushes exclusions per tick; harmless per tick, but see issue 9 for the aggregate effect.
