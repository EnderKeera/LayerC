# LayerC Simplification Audit

Scope: whole codebase behavior-preserving simplification (exact behavior, existing tests unchanged, verify each batch
with the full suite, commit incrementally). Status as of 2026-08-20, worktree
on branch main (post V2.4 Game Bar milestone).

## Rule for any change

1. Comprehend before touching (Chesterton's Fence); if you cannot explain the
   edge cases, do not edit.
2. Preserve output, errors, side effects, and ordering; tests must pass
   unmodified.
3. One verified change per step: build -> ctest -> commit.
4. Favour clarity over cleverness; avoid over-abstracting (a named helper that
   only hides one call site is not simpler).

## Done

- **[FIXED] Duplicated Win32 error-text helper.** `windows_error_text` was
  defined twice (anonymous namespaces in `src/platform/live_channel.cpp` and
  `src/gamebar/GameBarTransport.cpp`) and declared nowhere. Consolidated into
  a single canonical `layerc::platform::windows_error_text(DWORD)` in
  `src/platform/windows.cpp` (+decl in `windows.hpp`); both call sites now
  resolve there, and the host-side three-line suffix-trim logic is shared.
  Verified: full build, 10/10 ctest suites. Commit `5b0cc53`.
  Note: the two old copies differed only by an `ERROR_SUCCESS` guard that no
  caller ever reaches (set_error is never passed 0), so behavior is identical.
  `d3d_capture_support.cpp` `hresult_message` is NOT a clone (HRESULT ->
  UTF-8 std::string, different fallback text) and was left alone.

## Maintain, do not re-implement (duplication survey, verified 2026-08-20)

- **Logon-session check appears 4x**: `live_channel.cpp` (authenticate
  server + client), `GameBarTransport.cpp` (`same_session`),
  `worker/main.cpp`, `daemon/isolated_visual_worker.cpp`. Each is a short
  `ProcessIdToSessionId` compare on 2 PIDs. A shared
  `layerc::platform::same_logon_session` helper is possible, but it spans the
  isolated-worker auth boundary and worker/main uses the pattern inline with
  other checks. LOW priority: leave unless a natural home appears; touching
  the worker auth path is risk without much clarity gain.
- **Snapshot map-building loops** in `GameBarBridge::create_session_snapshot`
  (two identical `for -> unordered_map` loops over states and definitions).
  Extracting a `make_pointer_map` template is borderline over-abstraction;
  the current loops are self-evident. Leave.
- **`make_event()` / CreateEventW**: several event-pair constructions
  (`live_channel.cpp`, `GameBarTransport.cpp`, `daemon_app.cpp`,
  `daemon_visual.cpp`, `editor_visual.cpp`). Patterns differ enough
  (manual reset, auto reset, two-event vs one) that a shared helper would need
  options; not a clear win. Leave.

## Structural hotspots (recorded in review/layer20.review-2026-08-17.md;
re-verified line counts 2026-08-20)

These are the places where "simplify the codebase" actually lives. Each needs
a dedicated round with subsystem comprehension before edits; none should be
refactored blind.

1. **Files over the ~1000-line convention** (split only with tests green):
   - `src/editor/editor_automation.cpp` — 1496 lines
   - `src/core/automation_runtime.cpp` — 1280 lines
   - `src/core/automation_profile.cpp` — 1172 lines
   - `src/platform/profile_store.cpp` — ~700 lines (sidecar/codec area
     acceptable; do not split without a reason)
2. **json numeric storage** — `json` value stores numbers as `double`
   only; exact-integer round trips rely on care at call sites. Introducing an
   integer-preserving storage variant is a genuine simplification of a whole
   class of casts but is a parser/serializer-wide behavior change; it must be
   its own reviewed milestone with exhaustive round-trip tests.
3. **eval op-string dispatch** — automation evaluator dispatches on operator
   strings via a lookup instead of a typed enum; converting the matching is a
   mechanical but wide change (behavior-neutral), worth its own pass.
4. **sidecar naming drift** — `_visual`/automation/companion sidecar codecs
   were partially cloned under 'visual_' names; the `SidecarStore<CodecT>`
   template already consolidates load/write (verify no private clones remain
   before deleting anything).

## Suggested next batch (safe, small, verified)

- Re-scan for C4702/C5xxx warnings on a clean build; each real warning is a
  concrete dead-code/unreachable signal to simplify. (The editor_hid.cpp:281
  C4702 seen earlier was from a stale build state — rebuild to confirm.)
- Hunt for private functions defined but never called (link/dead-strip
  evidence) inside `src/core` and `src/daemon`.
