# One LayerE editor on FLTK: migration plan

**Status:** approved for trial implementation, 2026-11-16. Phase 0's gate passed on 2026-11-17 (commit `1487afc`); phase 1 is in progress. Support claims stay in [`README.md`](../README.md) and [`Linux.md`](Linux.md); design rules stay in [`DesignSystem.md`](DesignSystem.md); dated Linux editor evidence stays in [`LinuxEditorPlan.md`](LinuxEditorPlan.md).

## 1. Decisions

Made by the user on 2026-11-16. Changing one goes back to the user.

| # | Question | Decision |
| --- | --- | --- |
| D1 | Toolkit | **One FLTK 1.4 frontend for Windows and Linux.** The Win32/GDI frontend retires after the switch in §7. |
| D2 | Screen readers | **Pure C++, no AccessKit.** One toolkit-neutral model with two adaptors written directly against the platform protocols: UI Automation on Windows, AT-SPI over D-Bus on Linux. |
| D3 | Linux adaptor | **Direct AT-SPI on sd-bus**, replacing ATK. Drops ATK, the ATK bridge, libatspi, GLib in FLTK's loop and the 2.62 floor. |
| D4 | Metrics | **Scale from text size:** every metric is its Windows value × body text size / 12 px. Windows stays exactly as today; Linux's 14 px body gives 30 px controls. |
| D5 | Order | **Windows parity first:** after the build and accessibility phases, the FLTK editor reaches Windows feature parity before the visual kit and screens. |
| D6 | Switch | **Preview, then switch:** the FLTK editor ships beside the Win32 one on Windows in one tagged prerelease, then becomes the only editor after the user approves the evidence and switch. |
| D7 | Menus | **LayerE-drawn menus on both platforms**, including the menu bar, drop-downs and context menus. |
| D8 | Memory | **Windows: measure and report** private bytes and working set at every gate, with no ceiling. The approved Linux ceilings stay as they are. |
| D9 | FLTK version | **Pin 1.4.5 wherever this project builds FLTK:** vendored for Windows, and the Flatpak moves from 1.4.3 to 1.4.5. Native Linux packages keep the distribution's FLTK with a 1.4 floor. |
| D10 | Evidence | **Automated tests plus a separate agent review** at every gate. Physical screen-reader, IME and hardware runs are not gate requirements; they are listed as gaps (§9). |

## 2. Why

- **Two frontends.** Every editor change is built twice: the Win32 frontend (about 15,700 lines in `src/editor/`, with the GDI painters in `src/platform/win32/native_controls.cpp` and `native_theme.cpp`) and the FLTK frontend (about 12,200 lines in `src/editor/fltk/` after the phase 0 rename). Every v2.1 item under [Not yet in the code](DesignSystem.md#not-yet-in-the-code) names Windows and Linux files.
- **The Linux editor doesn't follow LayerE.** It is laid out in fixed coordinates for one 1080 × 790 window, with tabs, labels above fields and 28 / 30 px controls; it uses neither `Fl_Flex` nor `Fl_Grid`; `main.cpp` forces `FLTK_BACKEND=x11`, so a Wayland session shows it through XWayland, blurred at fractional scales; and FLTK's stock choices, tabs, browsers and colour chooser are only partly restyled.
- **FLTK is not the limit.** FLTK draws every control itself, which square, flat LayerE needs, and the [toolkit gate](LinuxEditorPlan.md#6-toolkit-selection-gate) measured it at about 7–9 MiB USS against the [approved Linux ceilings](Linux.md#linux-editor-resource-targets) of 20, 32 and 48 MiB. GTK 4 measured 55–65 MiB with Cairo and was rejected. FLTK has no screen-reader support of its own; this project already builds that itself.
- **Ubuntu 24.04 gets no native editor.** The ATK path needs ATK/AT-SPI 2.62 because older adaptors crash at cleanup ([Linux.md](Linux.md)); Ubuntu 24.04 ships 2.52, so its CI row runs `--editor off`. D3 removes that floor. Ubuntu 24.04 also ships FLTK 1.3, so phase 1 lets a Linux source build without the distribution's FLTK 1.4 statically link the pinned 1.4.5 (D9); native packages still need the distribution's FLTK 1.4.

## 3. Planning baseline

This records the pre-phase-0 baseline, not a claim about completed migration work.

- **FLTK.** Upstream's current stable release is 1.4.5 (25 April 2026), ABI-compatible with 1.4.0. The 1.4 branch is in maintenance; 1.5 is developed on `master` with no release schedule, and its roadmap mentions a likely new menu system. FLTK has no accessibility work planned beyond an old MSAA patch request. On Windows, FLTK 1.4 is per-monitor DPI aware (V2) by default and gives the window handle through `fl_xid()`. The Flatpak builds FLTK 1.4.3 from a checksum-pinned archive with the Wayland backend off.
- **The FLTK frontend is mostly portable already.** Only these files use Linux-only APIs: `binding_capture.cpp` (XI2), `calibration_window.cpp` and `visual_editor.cpp` (`visual_assets_linux`), `daemon_link.cpp`, `lease_worker.cpp`, `live_draft_client.cpp` (`eventfd`), `editor_tools.cpp` (`posix_spawn`), `tasks_window.cpp`, and `editor_appearance.cpp` (`profile_store_linux`).
- **The accessibility layer is tied to ATK.** `accessibility_semantics.hpp` includes `<atk/atk.h>`, and each `Node` holds `AtkObject` pointers for its parent and children. The bridge is about 1,400 lines across `accessibility_semantics`, `accessibility_tree`, `accessible_widgets` and `editor_accessibility`.
- **sd-bus is already a dependency.** The bus lane builds `layerc_bus` on `libsystemd` or `libelogind` (`cmake/LayerCLinuxBus.cmake`).
- **The Win32 frontend's platform reach.** It uses `SetWindowDisplayAffinity` (prompts, capture exclusion), `DwmSetWindowAttribute` and `SetWindowTheme` (dark chrome), Raw Input (`editor_hid.cpp`), `UiaRaiseNotificationEvent` (`accessible_text.cpp`), `IFileOpenDialog` and `GetOpenFileName`, the clipboard, `SPI_GETHIGHCONTRAST`, `WM_DPICHANGED` in six dialogs, `EnumWindows` (`window_discovery.cpp`), `ShellExecute`, `SHGetKnownFolderPath`, and Common Controls tooltips and list views. It has 13 dialog templates: prompt, panel item, export pack, import pack, runtime diagnostics, HID binding, automation, the main window, tasks, options, appearance, colour picker and shortcuts. The Game Bar companion isn't edited in the editor; profiles only carry its data.

## 4. Target architecture

```text
src/editor/common/        toolkit-free document, history, validation, view models (as today)
src/editor/fltk/          the one frontend (renamed from src/editor/linux in phase 0)
  kit/                    LayerE painters, metrics, layout, menus, keyboard (§6)
  a11y/                   model, tree pass, tests (§5)
    atspi/                AT-SPI adaptor on sd-bus (Linux)
    uia/                  UI Automation adaptor (Windows)
  platform/
    services.hpp          interfaces the frontend calls (below)
    linux/                implementations on src/platform/linux
    win32/                implementations on src/platform/win32
```

| Service | Linux implementation | Windows implementation |
| --- | --- | --- |
| Daemon link and live drafts | Unix socket client, `eventfd` wake (`daemon_link`, `live_draft_client`, `lease_worker`) | Named-pipe client on `framed_pipe` / `live_channel` (`live_edit_session` today) |
| Profile store and settings | `profile_store_linux`, XDG paths | `profile_store`, `settings_store`, Known Folders |
| Binding capture | XI2 in the capture dialog | Raw Input in the capture dialog; generic HID through `hid_device` |
| Detected tasks | EWMH scan on a demand-opened X11 connection | `window_discovery` (`EnumWindows`) |
| Visual assets and calibration | `visual_assets_linux`, the X11 calibration window | `visual_assets`, `calibration_session` |
| Open folder, external tools | `posix_spawn` of `xdg-open` | `ShellExecuteW` |
| File dialogs | `Fl_Native_File_Chooser` | `Fl_Native_File_Chooser` |
| Capture exclusion | Reported unavailable on X11 | `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` on `fl_xid()` |
| Window chrome | Window manager frame | DWM dark caption and square corners on `fl_xid()` |
| Contrast themes | System preset (FLTK palette) | `SPI_GETHIGHCONTRAST`, `GetSysColor`, repaint on `WM_SYSCOLORCHANGE` |

Interfaces carry no Win32, X11 or FLTK types; shared-core interfaces stay free of platform headers and handles. The services that exist in `src/platform/win32` are reused unchanged.

## 5. Accessibility

### 5.1 Model

A toolkit-neutral tree built by today's tree pass from FLTK widgets, in FLTK's own loop, with no thread. Per node: stable id, role, name, description, states (focusable, focused, selected, checked, mixed, expanded, collapsed, disabled, read-only, modal, showing, password), value or range (minimum, maximum, step), text with caret and selection for fields, actions, parent and children, relations (labelled-by, described-by), bounds in screen pixels, and pending announcements. Changes between passes become events.

| Model role | AT-SPI role | UIA control type |
| --- | --- | --- |
| application / window / dialog | application / frame / dialog | Window |
| push button | push button | Button |
| check box / radio button | check box / radio button | CheckBox / RadioButton |
| text field | entry; read-only: text; masked: password text | Edit |
| number field | spin button | Spinner, with an Edit child |
| drop-down | combo box | ComboBox |
| list / list row | list / list item | List / ListItem |
| menu bar / menu / menu item | menu bar / menu / menu item | MenuBar / Menu / MenuItem |
| check / radio menu item | check menu item / radio menu item | MenuItem |
| tab list / tab | page tab list / page tab | Tab / TabItem |
| scroll bar / slider | scroll bar / slider | ScrollBar / Slider |
| scroll pane, canvas | scroll pane / canvas | Pane |
| label / status line | label / status bar | Text |
| preview image | image | Image |
| group, slot | panel | Group |

The model has no expandable state or radio-group role: an adaptor reports expandable when a node is expanded or collapsed, reports a mixed check box as indeterminate, and treats a group whose direct children include a radio button as a radio group for selection. Masked fields publish no text.

### 5.2 AT-SPI adaptor on sd-bus (Linux)

- **Activation.** On the session bus, read `org.a11y.Status` (`IsEnabled`, `ScreenReaderEnabled`) and watch for changes. Only when one is true, get the accessibility bus address from `org.a11y.Bus.GetAddress`, connect, and build the tree; otherwise no tree and no cost.
- **Registration.** Call `org.a11y.atspi.Socket.Embed` on the registry with the editor's unique bus name and its root object path; the root implements `org.a11y.atspi.Application`, and the registry sets its `Id`.
- **Interfaces by role:** `Accessible` on every node; `Application` on the root; `Component` (extents, focus, hit testing); `Action` (buttons, check boxes, menu items); `Text` and `EditableText` (fields); `Value` (number fields); `Selection` (lists, tab lists, drop-downs, menus and radio groups).
- **Events:** `Object` state, property, children, text, caret and selection changes, `Announcement` for status lines, `Focus`, and `Window` activate and deactivate.
- **Cache:** `org.a11y.atspi.Cache` with `GetItems` and the add and remove signals, so Orca can fetch the tree in one call.
- **Loop:** the connection's file descriptor joins FLTK's loop through `Fl::add_fd`, and its timeout through `Fl::add_timeout`; no GLib loop runs. Reads are served from a tree no older than one refresh interval, rebuilt on demand without a listener.
- **Packaging:** the native package needs sd-bus (`libsystemd` or `libelogind`) instead of ATK, its bridge and libatspi; the Flatpak drops its pinned `at-spi2-core` module and keeps access to the accessibility bus. A desktop without either sd-bus library builds no editor and says why, as the ATK requirement does today.

### 5.3 UI Automation adaptor (Windows)

- **Hosting.** Each top-level FLTK window gets a fragment root. `WM_GETOBJECT` is sent rather than posted, so it never reaches FLTK's message loop; the adaptor subclasses the window (`SetWindowSubclass` on `fl_xid()`) and answers with `UiaReturnRawElementProvider`.
- **Providers:** `IRawElementProviderSimple`, `IRawElementProviderFragment` and `IRawElementProviderFragmentRoot`.
- **Patterns:** Invoke (buttons, menu items), Toggle (check boxes), Value (fields, drop-downs), RangeValue (number fields), Selection and SelectionItem (lists, radio groups), ExpandCollapse (drop-downs, menus), Scroll (lists), and Text with TextRange for fields.
- **Text pattern.** The required members are the document range, supported selection, `GetSelection`, `GetVisibleRanges`, `RangeFromChild`, `RangeFromPoint`, and the full text-range set (compare, expand, move by character, word and line, get text, bounding rectangles, select). It raises text-changed and text-selection-changed. This is the largest single piece and is proven first.
- **System caret.** UI Automation clients and Magnifier find the insertion point through the system caret. FLTK draws its own caret, so the adaptor keeps an invisible system caret (`CreateCaret`, `SetCaretPos`) at FLTK's caret position in the focused field.
- **Events:** focus changed, property changed (name, value, toggle, expand), structure changed, and `UiaRaiseNotificationEvent` for status announcements, at the moments the Win32 editor announces today. `UiaClientsAreListening` skips event work when nobody listens; providers disconnect when their window closes.
- **MSAA.** Windows bridges UI Automation providers to MSAA clients, so no separate MSAA code is written.

### 5.4 Tests

- Model tests on both platforms, from fixtures, without a display.
- Linux: the AT-SPI tree test (today's `linux_editor_accessibility_tests`) runs against the new adaptor under Xvfb with a private session and accessibility bus, unchanged in what it asserts.
- Windows: a test executable walks the tree through an `IUIAutomation` client in `ci.yml`: names, roles, values, focus order, text ranges and notifications for the main window and every dialog.

## 6. LayerE kit

- **Metrics (D4).** `tokens.json` keeps the Windows values. One source computes every metric as value × body / 12, rounded to whole pixels, so a 14 px Linux body gives 30 px controls and a 40 px row pitch. DPI scaling stays FLTK's per-monitor scale on top. The design-system checker learns the rule.
- **Layout.** A thin layer over `Fl_Grid` and `Fl_Flex` with the same calls `DialogLayout` offers (`field_row`, `row_label`, field caps), refitting on text-size and DPI changes. No fixed coordinates.
- **Painters:** button, field, drop-down, check box and radio, list row, scrollbar, tooltip, focus ring, status marks, key chips, colour button, and the overlay preview painter shared with the renderer's geometry.
- **Menus (D7).** LayerE draws the menu bar, drop-downs and context menus on both platforms. The kit implements Alt and F10 to the menu bar, Alt+letter, arrow keys, Esc, Shift+F10 and the menu-key, sentence-case labels with access keys, and the menu roles for both adaptors. FLTK's menu classes stay behind the kit, so a 1.5 menu rewrite changes one place.
- **Keyboard:** access-key underlines appear after Alt on Windows, as Windows does; Enter and Esc for primary and Cancel; Tab follows reading order.
- **Contrast themes:** Windows contrast themes as in [DesignSystem.md](DesignSystem.md#contrast-themes); Linux keeps the System preset.

## 7. Phases

Every phase ends on a green `ci.yml` for its exact commit, an agent review with no open high-severity finding (§9), and no change to the profile format. A phase lands as a series of reviewable slices on its working branch; each pushed slice gets its own `ci.yml` run so failures surface early, but only the green run for the phase's exact final commit closes the phase. From phase 0 the Win32 frontend takes fixes only; design work lands in the FLTK frontend.

The approved execution order is **0 → 1 → 2 → 3 → 4 → 7 → 5 → 6**. Phase 7 includes completing missing backend prerequisites under [Wayland.md](Wayland.md), not just changing the editor's window backend. Inspect current code and gates first; reuse and verify prerequisites already implemented. No Wayland support claim is made before its own qualification requirements pass, and Gamescope remains unsupported.

The §9 accessibility gate applies from phase 0 on both platforms. Necessary Windows UI Automation implementation and meaningful client/tree tests are brought forward from phase 1; an empty window alone does not satisfy that gate. There is no bootstrap waiver, stub coverage or substitution of local tests for hosted CI. Stop at a gate whose required evidence or authority is unavailable.

| Phase | Work | Ends when |
| --- | --- | --- |
| 0. Groundwork | Vendor FLTK 1.4.5 in `third_party/fltk` with `UPSTREAM.md` and a `THIRD_PARTY_NOTICES.md` entry, the way miniz is; its licence's exception allows static linking. Build it static, without OpenGL, as system headers so MSVC and MinGW stay warning-free. Move the Flatpak to 1.4.5. Rename `src/editor/linux` to `src/editor/fltk` and extract the platform services (§4) on Linux without behaviour change. Add a `layere_fltk` target on Windows that builds but doesn't ship, with the Windows accessibility implementation and client/tree tests needed for the full §9 gate. | Linux editor suites pass unchanged; `layere_fltk.exe` opens an empty window on `windows-2022`; both platforms' accessibility tree suites and all §9 gates pass. |
| 1. Accessibility | Model split; the AT-SPI adaptor replaces ATK on Linux; the UI Automation adaptor on Windows; the CI tree tests (§5.4). Ubuntu 24.04's CI row turns the editor on. | AT-SPI assertions unchanged and passing on Debian 13 and Ubuntu 24.04; the Windows tree test covers fields' text ranges. |
| 2. Windows parity | First the real editor startup on Windows (single instance, startup recovery), since every other row runs inside it; then the Windows services (§4) and every row of the Windows feature matrix (§8) in `layere_fltk.exe`, on today's widgets. | The matrix is all *implemented* on Windows; Windows editor tests run against the FLTK build. |
| 3. LayerE kit | Painters, metrics, layout, menus, keyboard and contrast themes (§6); the design-system changes in §10, with the checker. | Each component renders from the kit on both platforms and matches its design-system preview in reviewed screenshots. |
| 4. Screens | The main window (three slots, no tabs) and all 13 dialogs, on the kit, to [DesignSystem.md](DesignSystem.md) v2.1, both platforms together. FLTK's colour chooser goes. | The walkthrough passes on both platforms; Linux resource ceilings hold; the Linux-only items leave [Known deviations](DesignSystem.md#known-deviations-in-the-shipped-editor). |
| 7. Wayland prerequisites and window | Before the preview, complete missing V2.7 backend prerequisites under [Wayland.md](Wayland.md), then stop forcing X11 in the editor; X11-only services use an XWayland connection when one exists and otherwise report themselves unavailable. | Applicable backend and editor gates, including failure, fallback and cleanup paths, pass; physical qualification gaps remain explicit and no support claim precedes the owning requirements. |
| 5. Preview | Windows: the FLTK editor ships beside the Win32 editor in one tagged prerelease. Linux: the FLTK editor is the only editor, so it ships the new screens directly. | One tagged prerelease has been published with both Windows editors, its artifacts and evidence identify the exact commit, and reported issues are triaged. Tagging and publication require separate user approval. |
| 6. Switch | After the user approves the evidence and switch, `layere.exe` becomes the FLTK build. The Win32 frontend, `editor_resources.rc` and the GDI control painters are removed in one commit, tagged so it can be reverted; their tests move to the FLTK build. | Explicit user approval, the revertible switch commit and all phase gates; no automatic merge or publication. |

## 8. Windows feature matrix

Kept like the Linux matrix in [LinuxEditorPlan.md](LinuxEditorPlan.md): each Windows feature is *implemented*, *preserved but not editable*, *unavailable on this platform* or *deferred* in `layere_fltk.exe`. The starting rows, from the Win32 sources. The slice that implements a row records its status and Windows tests in the same commit, and the Phase 2 gate audits every row against the Win32 sources; a dash means not yet recorded.

| Area | Feature | Win32 source | Needs | Windows tests | Status |
| --- | --- | --- | --- | --- | --- |
| Profiles | List, new, duplicate, rename, turn on / off, delete, save, save as, open folder, game match, legacy upgrade | `editor_profiles`, `editor_shell` | Store service, `ShellExecuteW` | `layerc_windows_fltk_startup_tests` (startup, single instance, recovery), `layerc_windows_fltk_session_tests`, `layerc_windows_fltk_widget_tests`, `layerc_windows_fltk_autosave_tests` | *implemented*. Profile ID is editable text on Windows, as in Win32; Linux keeps it read-only |
| Profiles | Detected tasks | `editor_tasks`, `window_discovery` | Tasks service | `layerc_windows_fltk_model_tests`, `layerc_windows_fltk_widget_tests` | *implemented*, with Calibrate selected, row activation and Pick window |
| Cooldowns and bindings | Fields, groups, keyboard / mouse / gamepad capture | `editor_cooldowns`, `editor_dialog` | Windows focused capture adapter for physical scans, mouse input and XInput edges. Reached by running `layere_fltk.exe` directly until it ships beside `layere.exe` in Phase 5 | `layerc_windows_fltk_binding_tests`, `layerc_windows_fltk_widget_tests` | *implemented*; cooldown ID *preserved but not editable*, read-only as in Win32. No physical keyboard, mouse or controller observed |
| Bindings | Generic HID capture | `editor_hid`, `hid_device` | Foreground Raw Input dialog with advanced system-device opt-in and a validated local slot assignment. Reached by running `layere_fltk.exe` directly until Phase 5 | `layerc_windows_fltk_binding_tests` | *implemented*. No physical HID device observed |
| Panels | Items, bars, colours, split grouped bars, preview, Position on window | `editor_items`, `editor_bars`, `editor_preview_render`, `editor_color_picker` | Shared preview painter | `layerc_editor_overlay_preview_tests`, `layerc_windows_fltk_position_tests`, `layerc_windows_fltk_on_window_tests`, `layerc_windows_fltk_widget_tests`, `layerc_editor_color_tests` | *implemented*, with the overlay contrast line; unknown item kinds *preserved but not editable*, as in Win32. No live game overlay observed |
| Exchange | Import and export packs | `editor_packs` | File dialogs | `layerc_windows_fltk_session_tests`, `layerc_windows_fltk_widget_tests` | *implemented*; import review decides each row in turn. Select all and bulk Toggle are *deferred* to the Phase 4 import review screen |
| Runtime | LayerD status, live drafts, autosave, diagnostics, clipboard | `editor_live_edit`, `live_edit_session`, `editor_diagnostics` | Daemon-link service | `layerc_windows_fltk_daemon_link_tests`, `layerc_windows_fltk_autosave_tests`, `layerc_windows_fltk_widget_tests` | *implemented*, with the raw JSON view and Retry visual |
| Advanced | Automation, simulator, trace | `editor_automation*` | Shared editor tools; live trace uses the authenticated LayerD pipe on Windows | `layerc_windows_fltk_trace_relay_tests`, `layerc_windows_fltk_daemon_link_tests`, `layerc_windows_fltk_widget_tests` | *implemented*, with Validate, issue-to-JSON jump and Retry |
| Advanced | Visual triggers and calibration | `editor_visual`, `calibration_session` | Visual service | `layerc_windows_fltk_calibration_tests`, `layerc_windows_fltk_session_tests`, `layerc_windows_fltk_widget_tests` | *implemented*. No real capture source or GPU path observed |
| Options | Visual recognition, overlay and editor capture exclusion, launcher updates, text size | `editor_dialog` (Options) | Capture exclusion, settings | `layerc_windows_fltk_settings_tests` | *implemented* |
| Appearance | Presets, roles, contrast checks | `editor_appearance`, `appearance_contrast` | Shared contrast code | `layerc_windows_fltk_settings_tests`, `layerc_windows_fltk_model_tests`, `layerc_windows_fltk_widget_tests`, `layerc_editor_tests` | *implemented*. Apply refuses below the Text / Fill and Text / Selection floors in the Win32 editor's words, from one shared source. Role border, hover and disabled overrides are *preserved but not editable* until the Phase 3 painters draw those states per role. The advanced role tree and the picker's swatches, hex field and contrast line are *deferred* to Phase 4, which replaces FLTK's colour chooser |
| Interface | DPI changes, dark chrome, contrast themes, tooltips, shortcuts, screen readers | `native_theme`, `editor_shortcuts`, `accessible_text` | Kit (§6), UIA (§5.3) | `layerc_windows_fltk_settings_tests`, `layerc_windows_fltk_accessibility_tests`, `layerc_windows_fltk_text_tests`, `layerc_windows_fltk_patterns_tests`, `layerc_windows_fltk_ime_tests` (automated IMM checks; no installed CJK IME observed locally), `layerc_windows_fltk_widget_tests` | *implemented*, with the Windows shortcut table (Ctrl+D duplicates the cooldown). Win32's Show advanced toggle is *deferred* to the Phase 4 main window, Alt/F10 access-key menus to the Phase 3 menu kit. No physical DPI change or manual screen-reader session observed |

## 9. Evidence (D10)

- **Every gate:** the smallest CMake targets covering the change, `python tools/tests/test_design_system.py`, `python tools/check_design_system.py`, the accessibility tree tests on both platforms, screenshots of the affected screens reviewed against the design system, and memory: Linux USS against its ceilings, Windows private bytes and working set reported.
- **Agent review:** a separate agent reviews each phase's diff and evidence and writes `review/layerN.review-YYYY-MM-DD.md`; the phase closes when no high-severity finding is open, as the Linux toolkit gate did.
- **Gaps, stated as gaps:** Narrator, NVDA and Orca speech, IME composition with real Japanese, Chinese and Korean input methods, physical high-DPI monitors and real contrast themes are not observed by these gates. Release notes say the screen-reader trees are tested automatically and not with a physical screen reader. The phase 5 tagged prerelease is the first real-user exposure on Windows; the phase 6 switch weighs what it surfaced. These permitted gaps do not waive the physical qualification requirements for a Wayland support claim.

## 10. Design-system changes this plan makes

Applied with the code they describe, never ahead of it:

- **Metrics (D4):** the scale rule replaces `linux-input-height`, `linux-button-height`, `linux-margin` and `linux-group-gap`; Linux forms put labels beside fields, like Windows.
- **Linux main window:** the tabbed 1080 × 790 layout and its coordinate table go; both platforms use [Windows main window](DesignSystem.md#windows-main-window), renamed "Main window". The Tabs component is retired.
- **Menus (D7):** MenuBar and ContextMenu lose their Windows and Linux split.
- **Window frame:** WindowFrame keeps DWM on Windows and the window manager's frame on Linux.
- **Implementation map:** one FLTK column replaces the Windows and Linux columns.
- **Keyboard:** Ctrl+Plus, Ctrl+Minus and Ctrl+0 for text size on both platforms, as Linux has today.

## 11. Risks

| Risk | Mitigation |
| --- | --- |
| The Text pattern over FLTK fields reads caret, selection or words differently from a native edit | Phase 1 builds and tests it before any screen; the system caret is kept in step |
| Orca behaves differently without ATK | Phase 1 keeps today's AT-SPI assertions unchanged; Orca relies on AT-SPI, not on ATK |
| Windows keyboard conventions (Alt, F10, mnemonics) built by hand | The kit owns them, with tests in phase 3 |
| IME: FLTK's composition display and candidate placement on Windows | Tested in phase 2 with the system's input methods in automation; physical runs are a gap |
| FLTK 1.4 is in maintenance and 1.5 may change menus | 1.4.5 is pinned; menus sit behind the kit |
| Distributions without sd-bus (no systemd or elogind) | The editor is not built there and says why, as with ATK today |
| Two editors until phase 6 | The Win32 frontend takes fixes only from phase 0 |
| Translation units grow | Keep translation units focused and around 1,000 lines each |

## Sources

- FLTK: [1.4.5 announcement](https://groups.google.com/g/fltkgeneral/c/R4hDfhOycQg), [roadmap](https://www.fltk.org/roadmap.php), [platform notes](https://www.fltk.org/doc-1.4/osissues.html), [FLTK licence exception](https://spdx.org/licenses/FLTK-exception.html)
- UI Automation: [Text and TextRange control patterns](https://learn.microsoft.com/en-us/windows/win32/WinAuto/uiauto-implementingtextandtextrange)
- AT-SPI: [Socket](https://gnome.pages.gitlab.gnome.org/at-spi2-core/devel-docs/doc-org.a11y.atspi.Socket.html), [Cache](https://gnome.pages.gitlab.gnome.org/at-spi2-core/devel-docs/doc-org.a11y.atspi.Cache.html), [Application](https://gnome.pages.gitlab.gnome.org/at-spi2-core/devel-docs/doc-org.a11y.atspi.Application.html)
