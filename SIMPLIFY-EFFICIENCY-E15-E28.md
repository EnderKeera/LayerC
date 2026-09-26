# /simplify handoff — efficiency findings E15–E28 (detail)

`SIMPLIFY-HANDOFF.md` collapsed these into one line ("E15–E19, E21/E22/E28 CI,
E23–E27"). None of them have been applied. Full detail here so the next agent
doesn't have to re-derive them. Numbering matches the original efficiency
agent's report (E1–E14 and E20 are already covered in the main handoff).

## E15. Windows overlay drawing and override updates are wasteful per frame

- Where: `src/daemon/overlay_render.cpp:148-162`, `src/daemon/overlay_window.cpp:12-47,244-251`, `src/daemon/overlay_collection.cpp:69-76`, `src/daemon/daemon_overlays.cpp:31-61`
- Cost:
  - Per frame, per cooldown: a linear `find_if(states)` by id string, then `cooldown_status_text` plus `utf8_to_wide` allocations.
  - Every `OverlayWindow` copies all cooldown definitions.
  - Any override change runs `update_snapshot` on *every* panel: a re-copy, GDI font/bitmap/brush release and recreate, `SetWindowRgn` and `SetWindowPos`.
  - `hide()` frees the GDI resources, so hide-when-ready bars recreate the font and DC on each reappearance.
- Fix:
  - Index states by position; `CooldownEngine::order_` follows `profile.cooldowns`.
  - Rebuild the label only when tenths or charges change.
  - Update only the overridden panel, and separate content updates from geometry updates.
  - Keep GDI resources across `hide`.
- Confidence: high.

## E16. Linux overlay render allocates and copies per panel per frame

- Where: `src/daemon/linux/overlay_service.cpp:45,80-81,86-95`
- Cost: each panel per frame copies `Panel`, builds a `std::map`, and runs `cooldowns.update` followed by a by-value `snapshot(now)`, which allocates a new vector and repeats the update. Linear finds are made over both cooldowns and states.
- Fix: take one snapshot per session per render into the existing `snapshot_buffer`, index by position, and keep the panel copy only when an override applies.
- Confidence: high.

## E17. The template matcher redoes setup work every frame

- Where: `src/core/template_search.cpp:596-602,283`, `src/daemon/visual_capture_worker.cpp:189-230`
- Cost:
  - Per frame, per reference: builds a heap string key `sha + ":" + quality` and hashes it.
  - `rendered_sizes` (boundary vector, `unordered_set`, sort) is recomputed.
  - On the first use, `compact_template_image` copies an image the worker already compacted.
  - The prepared cache is cleared on every configure.
- Fix: prepare templates once in `apply_pending_configuration` and pass the prepared reference alongside the images. Cache rendered sizes keyed on (frame size, state).
- Confidence: high.

## E18. Trace publishing re-serializes the trace about 12 times per event

- Where: `src/core/automation_trace.cpp:220-239`, `src/daemon/daemon_automation.cpp:204-241`, `src/core/automation_runtime.cpp:21-24,234-239`
- Cost: while the editor is subscribed to traces, every automation event triggers a binary search that runs `export_layerc_trace` and `encode_live_message_json` on the prefix up to 11 times (2048 entries), then once more. The trace vector also erases from the front.
- Fix: serialize each entry once and pack greedily by cumulative encoded size. Use a `deque`.
- Confidence: high.

## E19. The editor polls the live channel every 16 ms whenever connected

- Where: `src/editor/editor_live_edit.cpp:194-198`
- Cost: it polls at 16 ms whenever connected, even with no edit session active: about 62 wakes per second while idle.
- Fix: wait on the channel's overlapped events with `MsgWaitForMultipleObjectsEx`, or use 16 ms only during an active drag and 250 ms otherwise.
- Confidence: high.

## E21. CI: the `linux-flatpak` job duplicates the native Linux build and tests

- Where: `.github/workflows/ci.yml:125-156`, `tools/run-linux-tests.sh:185-218`
- Cost: the job runs a full native Debug build, both ctest suites and the native smokes, duplicating the Linux matrix. Then `flatpak-builder` compiles everything again. It also downloads the Flathub Platform and SDK 25.08 every run with no cache.
- Fix: add a flatpak-only mode that builds just `layerc_linux_x11_smoke_fixture`. Cache the flatpak runtime directory and `.flatpak-builder` state, and add ccache.
- Confidence: high.
- Verify exact workflow/job/step names against the current `.github/workflows/ci.yml` before editing — per AGENTS.md, CI claims must be bound to the exact commit.

## E22. CI: `windows-analyze` repeats the full test run without a compiler cache

- Where: `.github/workflows/ci.yml:71-95`
- Cost: it repeats the full ctest run that `windows-msvc` already did; the analysis itself happens at compile time. It installs sccache but configures without the launcher or `SCCACHE_GHA_ENABLED`, and builds with `--parallel 2`.
- Fix: drop the ctest step and use the default parallelism. Enabling sccache here would only help if `/analyze` builds turn out to be cacheable — not verified.
- Confidence: medium.

## E23. Input matching re-checks every binding on every event

- Where: `src/core/input_matcher.cpp:22-34`
- Cost: every input event re-evaluates every binding's chord through `std::set` lookups. Every pressed token, bound or not, is inserted into a `std::set` with a node allocation.
- Fix: build a token-to-binding-index map, ignore unbound tokens, and re-evaluate only the affected bindings, using a flat set.
- Confidence: high (cost is modest).

## E24. Visual snapshots are copied twice per sample

- Where: `src/daemon/daemon_visual.cpp:134,161,194-199`
- Cost: `const auto captured = worker.snapshot()` copies the snapshot under the lock, then `push_back(captured)` copies it again because it is `const`. This happens per window per sample at 5–15 Hz and on each maintain pass, only to compute a status summary.
- Fix: make it non-const and move it, or expose a small status struct.
- Confidence: high.

## E25. The editor round-trips the profile through JSON on every change

- Where: `src/editor/editor_live_edit.cpp:66,273-286`
- Cost: `send_live_profile` runs on every recorded change, including text keystrokes. It serializes the profile, re-parses it locally, and the daemon then parses it again.
- Fix: drop the local re-parse (the daemon validates) and debounce live sends to about 30 Hz.
- Confidence: medium.

## E26. Global keyboard input is read from two sources

- Where: `src/daemon/daemon_input.cpp:173-187`, `src/daemon/daemon_session.hpp:96-98`
- Cost: global sessions get keyboard raw input through `INPUTSINK` and also poll `GetAsyncKeyState` + `MapVirtualKeyW` per key every 16 ms, so each event is processed twice.
- Fix: cache the VK codes at seed time. If the polling only exists to cover windows that UIPI blocks, run it only when the foreground has a higher integrity level.
- Confidence: medium — the polling may be intentional; confirm with a maintainer/AGENTS.md before removing.

## E27. The WGC session captures every frame the game renders

- Where: `src/capture/wgc_backend.cpp:177-183`
- Cost: the WGC session copies every frame at the game's refresh rate while the worker consumes at most 15 Hz.
- Fix: set `GraphicsCaptureSession.MinUpdateInterval` to 1/sample_rate where available (Win11 24H2+) and disable cursor capture. This API was recalled from memory, not verified against this repo's WGC ABI — check before relying on it.
- Confidence: medium.

## E28. CI: every job waits on the lint job

- Where: `.github/workflows/ci.yml:23,71,97,125`
- Cost: every job `needs: workflow-lint`, so all builds wait behind an actionlint docker pull.
- Fix: run lint in parallel and gate only `publish-rolling` on it.
- Confidence: medium (latency only).
- Verify exact job names against the current workflow before editing.

## Notes for whoever picks these up

- None of E15–E28 have been touched. No line numbers above have been
  re-verified against the current tree (some other findings in this pass did
  shift line numbers in files these findings also touch, e.g.
  `daemon_automation.cpp`, `overlay_render.cpp`) — re-read each file before
  patching.
- E21/E22/E28 (CI) require binding claims to the exact workflow content per
  AGENTS.md; don't assume the line numbers above still match
  `.github/workflows/ci.yml`.
- E26 and E27 are the two lowest-confidence items — read the surrounding code
  and, where noted, verify the platform API before changing behavior.
