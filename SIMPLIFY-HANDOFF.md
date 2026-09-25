# /simplify handoff — branch `simplify/codebase-cleanup`

Status of a whole-codebase `/simplify` pass (reuse, simplification,
efficiency, altitude). This file is a working note for the next agent; delete
it when the pass is finished.

## Verification state

- Windows (VS 2026, `build/`, Debug): warning-free, 13/13 CTest suites pass at
  this commit.
- Linux (WSL Debian, `~/layerc-build`, Ninja Debug): last built and passed
  (8 headless + 5 X11 under `xvfb-run -a`) **midway**. Edits after that run are
  NOT Linux-verified: `has_enabled_automations` use in
  `src/daemon/linux/input_service.cpp`, `src/calibration/linux_main.cpp`
  (`to_pixel_bounds`), `current_process_id` in `src/platform/linux/*`,
  `slugify`, `tests/linux_profile_store_tests.cpp` (TempDirectory +
  test_support read/write). Rebuild Linux first.
- Machine is memory-constrained. Build with:
  `cmake --build build --config Debug -- -m:1 -nodeReuse:false -p:UseMultiToolTask=true -p:CL_MPCount=2 -p:MultiProcMaxCount=2`
  (run from bash; PowerShell mangles `-m:1`).
- Editing: some files are CRLF. `ctx_patch` can silently no-op or mix line
  endings on CRLF files; use an EOL-aware script and verify diffs.

## Finding IDs

S = simplification, R = reuse, E = efficiency, A = altitude (from four review
agents). Duplicates: S14=R23=A27, S16=A22, R22=A1, R21⊂A10, S15≈A9, R24≈A7,
E20=A11, S10/S30/R27/A23.

## Done

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
  folded in (MinGW build not verified).
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
- R7 SingleInstanceGuard. R9 `quote_command_argument` (production sites;
  tests still hand-quote). R10 `platform::hresult_message`.
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

## Prepared but NOT applied

1. R22/A1 checkpoint policy: add
   `platform::checkpoint_automation_state(path, profile, values, due, now, force)`
   → `std::optional<ProfileWriteResult>` next to `write_automation_state`
   (ready → write → `automation_checkpoint_report`). Use it in
   `DaemonApp::quiesce_automation_runtime` (force), `retire_automation_session`
   (force; note: failure now slides `due` +1 s instead of staying), 
   `poll_automation_timer` (acknowledge on success), and
   `InputSession::checkpoint_automation_state` (Linux, keep fault message).
   Also fix trailing space in `automation_runtime_host_.timer_running = `.
2. R4 rest: add `write_file(path, span<const byte>)` and `read_bytes(path)` to
   test_support; delete local helpers in `profile_pack_tests.cpp`,
   `linux_profile_pack_tests.cpp`, `platform_live_test_support.*`
   (`write_test_file`/`read_test_file` → `test::write_file/read_file` in
   `platform_live_scenarios.cpp`), `visual_asset_load_contract.hpp`
   (`write_bytes`), `linux_x11_visual_tests.cpp` (`write_template`).

## Not started

- Simplification: S25 (`write_profile_bundle` create_only flag), S26 (drop
  `IRuntimeService::capability()` virtual + non-const `find`), S27 (merge
  automation_session.cpp into profile_session.cpp).
- Reuse: R2 byte/text cast helpers, R3 `EnumWireEntry` tables (runtime,
  control_channel, capture_probe), R5 automation expression builders, R6 test
  profile fixtures, R8 wide number/arg parsing, R11 `#RRGGBB` parse, R24
  sidecar tables, R26 X11 property reader, R27 remaining PS helpers.
- Altitude: A2 AutomationSession sink/profile, A3 continuous input to core,
  A4 panel override resolution in core, A5 shared visual evaluation plan,
  A6 single bundle planner, A7 one module registry, A8 editor save gating,
  A10 shared session registry, A11/E20 wait on reload events, A12 capability
  services, A13 capability status in runtime diagnostics (no `pop_back`
  splicing), A14 asset path without fake filename, A15 sidecar layout table,
  A18 visual serializer via `json::Value`, A19 input source ids, A20 one
  game-match policy (product choice), A21 simulator via AutomationSession,
  A24 guard topology, A25 `canonical_profile_key` ifdef, A29 shared pack tests.
- Efficiency: all E findings except none (E1 Linux reload on focus, E2
  automation early-out, E3 overlay SetWindowPos caching, E4 Game Bar 60 Hz,
  E5 window scan, E6 service reconcile gating, E7 XSync, E8 Linux idle
  repaint, E9 raw mouse registration, E10–E13 capture readback, E14 timer
  re-arm, E15–E19, E21/E22/E28 CI, E23–E27).

## Deliberately skipped

- A16 (parser duplicate range checks): changes which error is reported first.
- A23 (benchmark threshold helper): not shorter than explicit checks.
- A28 (live message field table): switch is compiler-checked per enum.
- R28 (Linux smoke/evidence command resolution): user vs system Flatpak
  scopes differ; untestable locally.
