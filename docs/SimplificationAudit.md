# LayerC Simplification Audit

Scope: whole-codebase behavior-preserving simplification (exact behavior, existing tests unchanged, verify each batch with the
full suite, commit incrementally). Verification date: 2026-08-20, branch main.

## Rule for any change

1. Comprehend before touching (Chesterton's Fence). If you cannot explain the
   edge cases, do not edit.
2. Preserve output, errors, side effects, and ordering; existing tests must
   pass unmodified.
3. One verified change per step: build -> ctest -> commit.
4. Favour clarity over cleverness; avoid over-abstracting (a helper that only
   hides one call site is not simpler) and avoid forcing churn in code that is
   already clear.

## Completed this pass (each commit verified: full build + 10/10 ctest)

- **Duplicated Win32 error-text helper** (commit `5b0cc53`).
  `windows_error_text` existed twice as anonymous-namespace clones
  (`src/platform/live_channel.cpp`, `src/gamebar/GameBarTransport.cpp`)
  and declared nowhere. Consolidated into one canonical
  `layerc::platform::windows_error_text(DWORD)` in `src/platform/windows.cpp`
  (+declaration in `windows.hpp`). The two old copies differed only by an
  `ERROR_SUCCESS` guard no caller can reach (set_error is never passed 0), so
  behavior is identical. `d3d_capture_support.cpp` `hresult_message` is NOT
  a clone (HRESULT -> UTF-8 std::string, different fallback) and was left alone.
- **Invalid `[[nodiscard]]` on function-pointer members** (commit `e953760`).
  `src/core/automation_ops.hpp` annotated two function-pointer members with
  `[[nodiscard]]`; that is not valid C++ (MSVC C4869), so MSVC emitted the
  warning on every build AND ignored the attribute. Removed the attributes
  (callers already check the bool), kept the intent in a comment.
- **One-shot loop masquerading as a range-for** (commit `e953760`).
  `src/editor/editor_hid.cpp` used `for (... : candidates) { ...; break; }`
  which can only run its first iteration, producing C4702 "unreachable code".
  Replaced with `if (!candidates.empty()) { auto &inferred = candidates.front(); ... }`.
- **Result: the whole build is warning-free** (`cmake --build ... -m:2` prints
  no `warning C...`), all 10 ctest suites pass unchanged.

## Surveyed and found already consolidated (earlier refactors) -- not open

These were flagged by older registers as structural debt but are already fixed
in current main; do not plan work against them:

- **json numeric storage**: `json::Value::Storage` is
  `variant<nullptr_t, bool, int64_t, double, ...>`; exact integers round-trip
  via `is_integer()/as_integer()`. (post-V2.3 commit `6d9bacc`)
- **evaluator op dispatch**: ops live in one typed registry
  (`automation_ops.hpp` `automation_ops` constexpr table) and resolve through
  typed `infer`/`evaluate` function pointers; `automation_runtime.cpp`
  dispatch is a typed `switch (ExpressionKind)`. (post-V2.3 commit `c10fe94`)
- **sidecar codecs**: `profile_store.cpp` drives all three sidecars
  (visual/automation/companion) through ONE `SidecarStore<CodecT>` template;
  no cloned load/write loops remain.

## Assessed and deliberately left (clarity vs churn / risk)

- **Logon-session check duplicated 4x**: `live_channel.cpp` (server+client),
  `GameBarTransport.cpp` (`same_session`), `worker/main.cpp`,
  `daemon/isolated_visual_worker.cpp`. A shared
  `layerc::platform::same_logon_session` is possible, but it spans the
  isolated-worker authentication boundary; the pattern is short and clear. Low
  value, nonzero risk -- leave.
- **Snapshot map-building loops** in `GameBarBridge::create_session_snapshot`:
  two identical `for -> unordered_map` loops. Extracting a helper is
  borderline over-abstraction on a 3-line pattern -- leave.
- **`make_event()` / CreateEventW sites**: events are constructed with
  differing manual/auto-reset and count semantics -- leave.
- **`hresult_message`**: distinct helper, intentionally different -- leave.

## Genuinely remaining structural work (dedicated rounds, each verified)

The only real hotspot left is file size / long-function sprawl. Splitting these
requires subsystem comprehension first (automation editor, automation runtime,
automation profile, pack import/export). Do not split blind; each split must
land with the full suite green and no include/registration regressions.

- `src/editor/editor_automation.cpp` -- ~1496 lines (largest; editor graph UI
  + simulator). Natural seam: simulator/state helpers vs view/dialog dispatch.
- `src/core/automation_runtime.cpp` -- ~1280 lines. Natural seam: evaluate /
  commit / trace regions.
- `src/core/automation_profile.cpp` -- ~1172 lines. Serializer / AST builder
  regions.
- `src/platform/profile_store.cpp` -- ~700 lines; acceptable but the
  bundle-transaction section and the sidecar sections could be split if a split
  improves reading, not just line count.
- (test files over 1000 lines such as `tests/platform_live_tests.cpp` are
  convention-consistent for harnesses; leave unless a real duplication is found.)
