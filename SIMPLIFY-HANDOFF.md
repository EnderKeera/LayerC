# /simplify handoff — branch `simplify/codebase-cleanup`

Status of a whole-codebase `/simplify` pass (reuse, simplification,
efficiency, altitude). This file is a working note for the next agent; delete
it when the pass is finished.

## Verification state

- Session 2 (cloud container, Ubuntu 24.04, GCC 13, `build/linux-dbg`, Ninja
  Debug): Linux is warning-free; 13/13 CTest suites pass (8 headless, 5 X11
  under `xvfb-run -a`); `tools/linux-runtime-smoke.sh` headless and x11 pass
  against the debug binaries. The session-1 Linux gaps are now verified.
- Windows was NOT built with MSVC or tested in session 2. Everything was
  compile-checked with a MinGW-w64 13 cross build from Ubuntu
  (`build/mingw-cross`, toolchain `x86_64-w64-mingw32-g++-posix`): no compile
  errors or warnings; only `layerd`, `layere` and `layerc_platform_tests`
  fail to link, on `HidD_*`/`HidP_*`, because Ubuntu's older mingw
  `hidsdi.h`/`hidpi.h` lack `extern "C"` (environment, not code). **First
  step on Windows: VS 2026 Debug build + all 13 CTest suites.** Windows-only
  runtime edits in session 2: `daemon_automation.cpp` (checkpoint helper,
  timer re-arm), `daemon_overlays.cpp` (core override resolution),
  `visual_assets.cpp` (asset directory), `canonical_profile_key`, editor
  color picker / number parsing, calibration and worker argument parsing,
  and test helpers.
- Windows machine is memory-constrained. Build with:
  `cmake --build build --config Debug -- -m:1 -nodeReuse:false -p:UseMultiToolTask=true -p:CL_MPCount=2 -p:MultiProcMaxCount=2`
  (run from bash; PowerShell mangles `-m:1`).
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

## Remaining (not started)

Mostly Windows runtime or cross-cutting refactors that need a Windows build
and test run to land safely:

- Altitude: A2 AutomationSession sink/profile, A3 continuous input to core,
  A5 shared visual evaluation plan, A6 single bundle planner, A7/R24 one
  module registry, A8 editor save gating, A10 shared session registry,
  A11/E20 wait on reload events (add the reload events to the daemon's
  `MsgWaitForMultipleObjectsEx` set), A12 capability services, A19 input
  source ids, A21 simulator via AutomationSession, A24 guard topology, A29
  shared pack tests (`profile_pack_tests` is Windows-only and assumes the
  unified store; sharing needs a split-store port).
- Efficiency (Windows): E3 overlay SetWindowPos caching, E4 Game Bar 60 Hz,
  E5 window scan, E6 service reconcile gating, E9 raw mouse registration,
  E10–E13 capture readback, E15–E19, E23–E27; CI: E21/E22/E28.
- S15/A9 rest (single `open_profile_store()` startup helper); R1 rest (raw
  `CloseHandle` sites listed above).

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
