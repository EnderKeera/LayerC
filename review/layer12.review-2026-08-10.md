# Layer 12 Review — Uncommitted Simplify + Capture Deduplication Patch

Date: 2026-08-10 — Uncommitted changes (23 files, `git diff HEAD`). Based on automated `/code-review` pass (10 findings, full verification). All paths relative to repo root.

## Summary

Second code-review sweep after Layer 11's visual-trigger hardening and the Layer 12 simplify pass that centralized JSON helpers (`json::append_unsigned` / `append_number` / `append_quoted`) and deduplicated capture bounds/black-frame logic into `window_capture.cpp`. Two silent-drop JSON serialization bugs and one stale-backup recovery bug are the only correctness blockers; the remainder are stale-state, behavioral-change, sampling, and maintainability/efficiency notes. No new injection or privilege issues.

---

## Bugs — requires action

### High

**1. `append_unsigned` / `append_number` silently drop value on `to_chars` failure → malformed JSON** `src/core/json_value.cpp:39` / `:48`

`to_chars` can fail (buffer too small, non-finite `double` like NaN/Inf for anchor values). Both helpers check `ec == errc{}` and otherwise append nothing. Caller `serialize_profile_json` / `serialize_visual_profile_json` / `live_edit` then emits `,"anchorX":,` or `,"schemaVersion":,` — invalid JSON. Live decode rejects as "must be finite number"; profile bundle on disk becomes unparseable, loader treats sidecar as invalid and `blocks_all_triggers()` suppresses all visual triggers.

*Failure scenario:* `panel.anchor_x = NaN` (or Inf) → `append_number` appends nothing → `"anchorX":,` → `json::parse` succeeds but `number` check fails → bundle write succeeds, next load `VisualSidecarState::invalid`.

*Fix:* On failure append a safe fallback (e.g. `"0"` / `"0.0"` / `"null"`) or return `bool`/`std::expected` and make serializer return an error; at minimum assert and append `"0"`. Centralizing via `json::append_number` is the right altitude — just harden the contract.

### High

**2. Early `sidecar_backup` refresh failure leaves stale `.bak` → stale recovery** `src/platform/profile_store.cpp:389`

`write_profile_bundle` replaces primary visual, succeeds writing base, then refresh of `sidecar_backup` fails. `restore_base_and_visual(false,false)` rolls back `path` and `.sidecar` but leaves old `.bak` on disk (refresh was to overwrite stale backup). Next load with absent/invalid primary finds valid stale backup → recovers stale visual rules, resurrecting old assignment after a failed commit.

*Failure scenario:* `profile_store` writes `profiles\{id}.json` OK, `profiles\_visual\{id}.json` OK, `profiles\_visual\{id}.json.bak` refresh `ReplaceFile` fails (ACL/lock) → rollback path+sidecar, but `.bak` retains pre-write content → restart loads `(absent primary, valid stale .bak)` → stale.

*Fix:* On sidecar-backup refresh failure also restore or delete `.bak` (delete if previous primary was invalid, else restore previous `.bak` temp). Mirror the base-backup discipline.

---

### Medium

**3. Pixel measurement cache keys by region+predicate, not quality — hidden single-quality-per-worker assumption** `src/daemon/visual_capture_worker.cpp:24`

`evaluate_once` caches `(region, predicate, availability, matched)` without `CaptureQuality`. Currently `daemon_visual` filters to one quality per `(HWND, quality)` worker, so the bug is latent. If mixed qualities coexist or the filter is removed, a Low 64-px grid result is reused for Balanced 256-px / Exact exact on the same frame → opposite `matched`, spurious or suppressed triggers.

*Fix:* Key by `(region, predicate, quality)` or by `rule_id + quality`, or assert/document `one quality per worker` invariant and add a `quality` check to `find` in the dedup.

**4. `poll_visual_captures` reuses pre-evaluation snapshot for status — one-poll lag** `src/daemon/daemon_visual.cpp:142`

Snapshot taken at poll start (`availability = unavailable` / `protected_or_black`), capture poll then succeeds to `available`, evaluations trigger, but `publish_visual_status` reuses stale snapshot → tray still shows "capture unavailable" until next `update_visual_status` tick. User-visible status and `live_channel` `runtime_status` lag one interval.

*Fix:* Re-read `worker->snapshot()` after `poll`/`on_measurements` before `publish_visual_status`, or publish from the fresh snapshot.

**5. Calibration `quoted` wrapper path now via `json::append_quoted` — still no `to_chars` check in live protocol decimal path** `src/core/live_edit.cpp:312`

Parallel to #1: `encode` builds quoted decimal strings for `revision` / `editSession` / `targetWindow` via `append_unsigned`. On `to_chars` failure it emits `""` empty string → `decimal_string_field` rejects "must be an unsigned decimal string" → live message dropped as protocol error, edit session stalls and editor retries.

*Fix:* Same hardening as #1 — make `append_unsigned` infallible (it is for `uint64_t` with 32B buffer) or propagate error.

---

### Low-Medium

**6. `frame_is_nearly_black` sparse sampling can miss bright content** `src/capture/window_capture.cpp:23`

Samples at most 4096 pixels via strided sampling (`step = size/4096`). Large 4K window with 1 px bright border or small icon → bright pixels skipped → `bright*1000 < sampled*5` true → classified `protected_or_black` → pixel rules forced `unavailable` until next non-black frame.

*Failure scenario:* 3840×2160 window, 1-px white border (≈0.1% bright pixels), step skips border → false black → triggers blocked.

*Fix:* Accept as heuristic, or sample border pixels explicitly, or note limitation; not a blocker for the simplify.

**7. `require_selected_task` tightens HWND acceptance vs old `use_selected_task`** `src/editor/editor_tasks.cpp:151`

Adds `IsWindow` guard. Task enumerated then closed before "Use Selected Task" click: old code applied matcher from candidate data, new code shows "Select a visible task first." — observable behavioral change (requires re-enumeration).

*Impact:* Minor UX tightening; arguably more correct. Document or keep.

---

### Maintainability / Duplication

**8. Duplicated capacity check in `has_rule_capacity` lambda** `src/editor/editor_visual.cpp:587`

Lambda captures `merged` by reference, condition re-checks size after `push_back`, hard-codes "four" in MessageBox. Raising `max_pixel_rules` from 4 to 8 requires updating condition and string; future call site may forget check, allowing 5th rule past `validate_visual_profile` → "Calibration rejected" after commit.

*Fix:* Extract `constexpr` + helper `bool can_add_rule(const VisualProfile&)` and share message constant `kMaxRules`.

**9. Registry read/write split — `write_dword` centralized but read still duplicates `RegCreateKey`** `src/platform/windows.cpp:46` / `:62`

`write_dword` is now single helper; `read_default_true_dword` still inlines `RegCreateKeyEx` with its own access mask / WOW64 flags. Future virtualization fix applied only to `write_dword` diverges → theme/accent appears saved but reverts after restart.

*Fix:* Extract `open_layerc_key(bool write)` shared helper.

---

### Efficiency

**10. Measurement dedup via linear `find_if` over vector `O(N·R)` per poll** `src/daemon/visual_capture_worker.cpp:161`

`Window shared by 8 profiles × 4 rules = 32 evaluations/poll at 15 Hz → 1024 comparisons/s` in hot capture thread via `find_if` on vector. Hash map / `unordered_map` keyed by `(region,predicate)` would be `O(N)`. Also retains enclosing scope via lambda capture.

*Fix:* `std::unordered_map<CacheKey, index>` or sort+dedup before evaluate. Only matters under heavy multi-profile sharing (global + per-game).

---

## Verified clean / no action

- JSON helper centralization (`profile_serialize` / `visual` / `calibration` → `json::append_quoted` / `append_number`) is correctly altituded at `layerc::core::json`, removes `<array>`/`<charconv>`/`<limits>` duplication, and removes manual `append_unsigned` loop in favor of `to_chars` (follow-up hardening in #1/#5 aside).
- Capture deduplication (`capture_window_bounds` / `frame_is_nearly_black` → `window_capture.cpp`) removes `dwmapi.h` includes from backends and merges `capture_bounds` + `nearly_black` duplicates — net -~40 LOC, no behavioral shift.
- `overlay_render.cpp:33` `append_unsigned(std::wstring&)` is `wstring`-specific (GDI label), not interchangeable — correctly left out of JSON reuse.
- `visual.cpp:373` `color_text` (`#RRGGBB`) stays local (distinct format).
- `daemon_visual` quality promotion and `live_edit` freshness window unchanged from Layer 11 — still intentional.

---

## Suggested next steps

1. Harden `json_value.cpp:39` / `:48` and `live_edit.cpp:312` to be infallible or error-propagating (High #1, #5) — single fix in `json_value.cpp`.
2. Fix stale `.bak` survival on backup-refresh failure `profile_store.cpp:389` (High #2).
3. Key measurement cache by quality or assert single-quality-per-worker `visual_capture_worker.cpp:24` (Medium #3).
4. Refresh snapshot before `publish_visual_status` `daemon_visual.cpp:142` (Medium #4).
5. Extract `open_layerc_key` and `can_add_rule` helpers (Low #8, #9) and consider hash dedup (Low #10).

---

## Appendix — raw code-review output (2026-08-10 automated pass)

```
src/core/json_value.cpp:48              append_unsigned/append_number silently drop value on to_chars failure
src/daemon/visual_capture_worker.cpp:24 pixel cache not keyed by quality
src/platform/profile_store.cpp:389      sidecar .bak stale after early failure
src/editor/editor_tasks.cpp:151         IsWindow guard tightens HWND acceptance
src/daemon/daemon_visual.cpp:142        status reuses stale pre-evaluation snapshot
src/capture/window_capture.cpp:23       strided black check can miss sparse bright pixels
src/core/live_edit.cpp:312              decimal quoted string via unchecked append_unsigned
src/editor/editor_visual.cpp:587        has_rule_capacity lambda duplication
src/platform/windows.cpp:46             registry write centralized but read still duplicates
src/daemon/visual_capture_worker.cpp:161 linear dedup O(N*R) per poll
```

Each entry above expands the corresponding raw finding with line-reverified context and failure scenario.
