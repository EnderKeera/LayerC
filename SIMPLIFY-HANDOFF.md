# /simplify handoff — branch `simplify/codebase-cleanup`

Status of a whole-codebase `/simplify` pass (reuse, simplification,
efficiency, altitude). This file is a working note for the next agent; delete
it when the pass is finished.

## Verification state

- Session 3 (cloud container plus the maintainer's self-hosted runner):
  - Linux (Ubuntu 24.04, GCC 13, `build/linux-dbg`, Ninja Debug) before
    every push: warning-free, 8/8 headless and 5/5 X11 (`xvfb-run -a`)
    CTest suites.
  - MinGW-w64 cross compile before every push: no compile errors or
    warnings; only the known `HidD_*`/`HidP_*` link failures (Ubuntu's
    mingw `hidsdi.h`/`hidpi.h` lack `extern "C"`).
  - Windows: `windows-selfhosted.yml` on the `layerc-bench` runner, VS 2026
    Debug build and all 13 CTest suites. Finished runs: 36211860428 on
    `28d43c6` (R1, E3, E9, E5) 13/13; 36212546696 on `79025ff` (A19, A3)
    13/13. Later commits are reported only from their own finished runs.
  - No interactive or hardware check was done. E9 (mouse Raw Input) still
    needs a manual check: a mouse binding and a mouse automation rule must
    still fire.
- Session 2 (Linux verified; Windows MinGW compile-check only) and session 1
  (reported by its author as VS 2026 Debug, 13/13) are superseded by the
  session-3 Windows runs above for everything that landed through
  EnderKeera/LayerC#8.
- Windows machine is memory-constrained. Build with:
  `cmake --build build --config Debug -- -m:1 -nodeReuse:false -p:UseMultiToolTask=true -p:CL_MPCount=2 -p:MultiProcMaxCount=2`
  (from bash; in PowerShell quote each switch, e.g. `'-m:1'`).
- Editing: a Windows checkout has CRLF files. `ctx_patch` can silently no-op
  or mix line endings on CRLF files; use an EOL-aware script and verify diffs.

## Finding IDs

S = simplification, R = reuse, E = efficiency, A = altitude (from four review
agents). Duplicates: S14=R23=A27, S16=A22, R22=A1, R21⊂A10, S15≈A9, R24≈A7,
E20=A11, S10/S30/R27/A23.

## Done in session 1

- S1 dead daemon automation helpers (`next_origin`, `active_event`,
  `DaemonApp::automation_event`) → `AutomationSession::event` directly.
- S2/S18 editor: `ProfileDocument` stores `VisualSidecarLoad` /
  `AutomationSidecarLoad` directly; dead `direct_drag_enabled_`,
  `automation_error`, `set_legacy_base_schema` removed.
- S3 dead `load_current_*_profile`, `refresh_template_mask_preview`;
  `visual_sidecar_is_legacy` file-local.
- S4 dead `action_accepts_value`, `effect_order()`,
  `observation_stable_samples`, `x11_device_kind_label`
  (kept `consecutive_black_frames`: tests use it).
- S5 `CapturePhase` removed. S6 isolated decoders removed; one
  `encode_sequence_packet`. S7 shadowing `blocks_all_triggers` removed.
- S8 `panel_geometry`/`bar_geometry` metrics param + `PanelMetrics` removed.
- S9 `AutomationActionSpec::accepts_value()` derived from value policy.
- S10/S27/S30 PowerShell: `Invoke-Checked` removed, single
  `Resolve-LayerCBuildRoot`, shared `Assert-LayerCExecutables`,
  table-driven `run-hardware-benchmarks.ps1`.
- S11 single `core::product_version` (no `LAYERC_VERSION`,
  `runtime_package_version`, `package_version`).
- S12 accessor-only classes → plain fields: `EditorShell`, `PanelPreview`,
  `PackDialogs`, `SessionRegistry`, `LiveEditHost` (structs); public fields on
  `CooldownEditor::list`, `TaskPicker`, `InputRuntime`, `VisualCaptureRuntime`,
  `AutomationRuntimeHost`, `OverlayRuntime`, `CompanionHost`.
- S13 `OverlayEventSink` and `AutomationHost` interfaces removed (friends).
- S14/R23/A27 `platform::try_*` UTF-8 forwarders removed; `editor::to_utf8`,
  `control_text`, `color_ref`, `rgb_value` replaced by platform functions.
- S15/A9 (partial) `commit_profile_transaction(dir, …)` and
  `recover_profile_transactions(dir)` overloads; 6 call sites use them.
  NOT done: single `open_profile_store()` startup helper.
- S16/A22 Game Bar: `GameBarAppServiceClient.cpp` MSVC-only,
  `GameBarAppServiceClient_stub.cpp` for MinGW, `GameBarRetryPolicy.cpp`
  folded in (MinGW cross-compile verified in session 2).
- S17 one `X11Dispatch` struct. S19 `WindowCaptureSession` without pimpl,
  explicit WGC→DD fallback. S20 `BoolSetting` + `read/write_bool_setting`.
- S21/S22/S23 CMake: `layerc_win32_exe`, redundant include dirs/features
  removed, `layerc_daemon_runtime_services` removed, widget core sources
  derived from vcxproj, unreachable `NOT MSVC` branch removed.
- S24 `Visual/Automation/CompanionSidecarState` aliases → `ModuleSidecarState`.
- S28 `drain_events` removed; event loop rebuilds poll list each wait.
- S29 `EditorApp` → `run_editor`; `show_notice` param removed.
- R1 (partial) `platform/win32_handle.hpp` (`OwnedHandle`,
  `SingleInstanceGuard`) used in window_query, window_discovery, worker main,
  live_channel peer check, isolated helper (`BoundedHelper` members),
  daemon/editor single instance, `start_daemon`, `VisualCaptureRuntime` and
  `AutomationRuntimeHost` events. Remaining raw `CloseHandle`: durable_file,
  daemon_app reload events, calibration main, daemon_diagnostics, hid_device,
  profile_store, profile_file_guard, visual_assets, tests.
- R7 SingleInstanceGuard. R9 `quote_command_argument` (production sites).
  R10 `platform::hresult_message`.
- R12–R16 theme helpers in `native_theme` (fill/frame/outline, dialog font,
  `native_theme_colors(appearance, role)`), `window_text`/`window_class`.
- R17 `process_image_path`, `same_session_as_current_process`.
- R18 `platform::current_process_id()` (removed `durable_file_internal.hpp`,
  `file_guard_process_id`). R19 visual-asset GC uses `is_lowercase_sha256`.
- R20 `read_template_asset_bytes`. R21 `core::has_enabled_automations`.
- R25 `json::exact_unsigned_value`. A17 named bar/charge limits in
  `types.hpp`. A26 calibration uses `to_pixel_bounds`. A30 `core::slugify`.
- R4 (partial) `test::write_file/read_file` in `tests/test_support.*`;
  linux_profile_store_tests uses them and `test::TempDirectory`.

## Done in session 2

- R22/A1 `platform::checkpoint_automation_state` used by
  `quiesce_automation_runtime`/`retire_automation_session` (force; a failed
  retire write now slides `due` one debounce like every other path),
  `poll_automation_timer` and Linux `InputSession`.
- R4 rest: every test uses `test::write_file/read_file` (string-based; no
  byte overloads were needed); one-shot `std::ofstream` blocks removed.
  R9 rest: tests use `quote_command_argument`.
- S25 `write_profile_bundle_create_only(path, profile)` (dead module params
  removed). S26 registry slot is the capability: `IRuntimeService::capability()`,
  the mismatch check (and its test) and non-const `find` removed.
- R2 `core::text_bytes/owned_bytes/as_text` (utf8.hpp) replace byte↔text
  casts in platform, bundle, ctl, frame decoder and tests.
- R3 `capability_wire`, `service_state_wire`, `control_command_wire`
  `EnumWireEntry` tables; `capture_probe` reuses `capture_availability_wire`
  (its `"wgc"` backend spelling is kept: benchmark evidence embeds it).
- R5 `core::expr::literal/variable/binary` replace editor/test/tool copies.
- R8 `platform::ascii_narrow` + `parse_integer<T>` (text.hpp) replace the
  editor, worker, calibration and capture-probe wide integer parsers.
- R11 `json::parse_color_text`; editor/calibration labels use `color_text`.
- R26 `x11_property.hpp` shared by window tracking and the EWMH probe.
- A4 `core::resolve_panel_overrides` used by both overlay renderers.
- A14 `unified_visual_asset_directory` + `visual_asset_file` (no fake
  `asset.profile.json`). A15 `module_file_path` table for sidecar/state
  paths. A25 `canonical_profile_key` via `path_to_utf8` +
  `preferred_separator` (no `#ifdef`).
- E1 Linux window/device changes rematch sessions without rescanning the
  store (store rescans on control reload/import notification only; sidecars
  and automation state are still loaded per refresh). E7 `XSync` calls the
  error trap already performs removed. E14 automation timer re-armed by
  `SetTimer` replacement instead of Kill+Set.

## Done in session 3

- R1 rest: durable_file, daemon diagnostics, hid_device, platform_events,
  profile_store, visual_assets, the transaction mutex and the daemon reload
  events use `OwnedHandle`; close points that matter stay explicit via
  `reset()`.
- E3 background overlays skip `SetWindowPos` when already visible at the
  target rect (checked against the real window rect, so moves by others are
  still repaired). E5 the 1 s window scan is skipped when no enabled window
  profile exists. E9 mouse Raw Input is registered only while a mouse
  binding, a mouse-capable press/release automation rule or a system-class
  HID binding needs it.
- A19 `core::input_source_id`, `controller_axis_source_id`, `hid_source_id`,
  `source_filter_names_device` (byte-identical ids, pinned by a core test).
  A3 `AutomationSession::input_value_event` owns the continuous-input policy
  (core tests cover demand, deadzone, rate limit, threshold, momentary).
  A2 `AutomationSession` owns its `profile`; the per-call `base` parameter
  is gone (the sink stays per call because the Linux sink binds to a
  session object that moves). A7/R24 `core::ProfileModuleKind` replaces
  `platform::ModuleSidecarKind` and the bundle's `SidecarKind`.
  A8 `EditorWindow::require_stored_profile` gates live edit and
  calibration.
- CI: `windows-selfhosted.yml` (manual dispatch only, SHA-pinned, quoted
  MSBuild switches, `pwsh -NoProfile`); `ci.yml` cancels superseded pull
  request runs.

## Remaining

Nothing. Every finding is done above or has a recorded reason below.

## Deliberately skipped

- A16 (parser duplicate range checks): changes which error is reported first.
- A23 (benchmark threshold helper): not shorter than explicit checks.
- A28 (live message field table): switch is compiler-checked per enum.
- R28 (Linux smoke/evidence command resolution): user vs system Flatpak
  scopes differ; untestable locally.
- S27 (merge automation_session.cpp into profile_session.cpp): the policy
  header is deliberately light (platform includes it without the session's
  engine headers); both files are small and cohesive.
- R6 (shared test profile fixtures): fixtures differ per suite (ids, keys,
  charges, groups); sharing couples suites to save ~3 lines each.
- R27 (remaining PowerShell helpers): no duplicate functions remain; inline
  edits are unverifiable without PowerShell.
- A13 (Linux status via `json::Value`): `Value::Object` is a sorted map, so
  it reorders `layerctl status --json` keys (published layout, substring test).
- A18 (visual serializer via `json::Value`): changes key order/layout of every
  stored visual sidecar; an on-disk format change, not a cleanup.
- A20 (one game-match policy): product choice.
- E2 (automation early-out): skipping evaluation also skips `poll_ready`'s
  silent edge update, changing when ready transitions fire.
- E8 (Linux idle repaint): the 100 ms repaint is the only Expose repair for
  override-redirect overlays; needs Expose handling plus physical X11 evidence.
- S15/A9 rest (single `open_profile_store()`): the Windows programs only
  recover transactions, the Linux daemon first ensures the split XDG roots,
  and layerctl ensures roots only on write commands; each program formats
  its own error, so a helper would wrap two calls.
- R1 in `live_channel`: the pipe uses an `INVALID_HANDLE_VALUE` sentinel
  inside the overlapped I/O state machine and its events feed OVERLAPPED
  structures at about 15 sites. Test fixtures keep raw `PROCESS_INFORMATION`
  handles.
- A5 (shared visual evaluation plan): the evaluator is already shared; the
  plans differ in rate policy (Windows clamps to 5–15 Hz on a capture thread
  or the isolated worker, Linux uses max(1, rate) in its loop).
- A6 (single bundle planner): already done; preview, dry-run and commit use
  one `build_import_plan`.
- A10 (shared session registry): Windows keeps one session per
  profile × window, Linux one per profile; sharing picks an instance model.
- A11/E20 (wait on reload events): the 1 s scan timer must stay, so the
  only effect is faster reloads, a behaviour change.
- A12 (capability services): covered by the typed `OwnedCapabilityService`
  adapter; the remaining switches are compiler-checked.
- A21 (simulator via AutomationSession): would commit cooldown effects and
  add ready transitions, changing the trace the editor shows.
- A24 (guard topology): the per-platform guard code is filesystem semantics
  and on-disk journal names, inside the durable transaction path.
- A29 (shared pack tests): needs a split-store port of the fixture and
  platform-specific expectations (Windows reserved names); Linux already has
  its own pack contract test.
- E4 (Game Bar 60 Hz): no such path in the current code; snapshots publish
  on the 1 s scan timer and the widget only has a one-shot stale timer.
- E6 (service reconcile gating): reconcile also re-applies session-dependent
  state (input registration follows active sessions) under unchanged demand.
- E10–E13 (capture readback): readback already reuses its staging texture
  and copies only sampled rows; the rest needs `layerc-bench` benchmarks or
  changes frame latency (async staging).
- E15–E19, E21–E28: their descriptions were not preserved by session 1 and
  cannot be recovered from the tree or history. A fresh CI review found one
  inefficiency (superseded PR runs kept running), fixed above.
