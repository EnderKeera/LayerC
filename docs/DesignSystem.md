# LayerE Design System

The visual language of the LayerC editors: Windows `layere.exe` (Win32/GDI)
and Linux `layere` (FLTK on X11). This document owns the editor look; source
owns exact behavior. Behavior, support claims and evidence stay in
[`README.md`](../README.md) and [`Linux.md`](Linux.md).

**Version:** v1, "square native" (2026-10-25).
**Implemented in:** the single commit tagged `design-v1-square-native`
(editor: square native-font restyle for Windows and Linux LayerE), which also
adds this document and the matching walkthrough expectations.
**Previous look:** its parent, tagged `design-v0-rounded` (rounded corners,
14 px Segoe UI Variable Text, 30 px controls; FLTK default grey on Linux).
See [Revert or reimplement](#revert-or-reimplement).

**Scope:** editor chrome only — the main window, dialogs, the calibration
window and their controls. Out of scope: the in-game overlay
([`src/daemon/overlay_render.cpp`](../src/daemon/overlay_render.cpp),
Linux overlays), whose shape follows each profile's own `corner_radius`;
the editor preview of that overlay (it must stay faithful to the profile);
and the Game Bar XAML widgets.

## Principles

1. **Square.** Every control, focus ring and window frame has square
   corners. Only radio buttons are round, because roundness is their meaning.
2. **Native type.** Text uses the platform's own interface font at its own
   size, never a bundled or "web" face.
3. **Flat.** Solid fills inside 1 px outlines; no bevels, gradients or
   shadows. Depth comes from the neutral ramp, not effects.
4. **One accent.** The accent marks state — the primary action, selection,
   focus and checked state — and nothing decorative.
5. **One rhythm.** One control height and an 8 px spacing grid per platform.
6. **Same product on both platforms.** Linux uses the same ramp, presets and
   roles as Windows; toolkit differences are confined to the adapters below.

## Color tokens

One neutral dark ramp (Windows 11 dark values) shared by every preset;
presets differ only in their accent. Source:
`layerc::platform::neutral_theme` in
[`native_theme.hpp`](../src/platform/win32/include/layerc/platform/native_theme.hpp)
and the mirrored constants in
[`editor_appearance.cpp`](../src/editor/linux/editor_appearance.cpp).

| Token | Value | Use |
| --- | --- | --- |
| `background` | `#1F1F1F` | Window, menu bar, status strip, labels' fill |
| `panel` | `#2D2D2D` | Neutral button fill; Linux unselected tabs |
| `input` | `#171717` | Edits, lists, profile sidebar, preview |
| `border` | `#6E6E6E` | Windows control outline (enabled) |
| `divider` | `#333333` | Disabled outlines, menu-bar and status lines |
| `text` | `#F3F3F3` | Primary text |
| `secondary` | `#B8B8B8` | Labels, status text, "Off" tags, executable names |
| `disabled` | `#808080` | Disabled text and glyphs |
| `button_hover` | `#383838` | Button and menu-bar hover |
| `row_hover` | `#2A2A2A` | List row and drop-down hover |
| `pressed` | `#262626` | Pressed neutral button |

| Preset | Accent |
| --- | --- |
| Graphite / Copper (default on both platforms) | `#D88E4B` |
| Graphite / Teal | `#5AB6AA` |
| Graphite / Plum | `#B382E2` |
| Graphite / Slate (`dark`) | `#8FA3B8` |
| System | Windows: the Windows accent color. Linux: FLTK's own palette |

Derived colors (computed, never hard-coded elsewhere):

- **Selection:** `blend(background, accent, 60/255)` — a quiet tint that keeps
  primary text above 7:1; list rows add a 3 px accent rail.
- **On-accent text:** `#141414` when the accent's luma
  (`299R + 587G + 114B`) is at least 140000, else `#FFFFFF`.
- **Primary button:** fill = accent; hover `blend(accent, text, 40)`; pressed
  `blend(accent, fill, 60)`.
- **Linux outline:** `control_border(fill)` = fill blended 84/255 toward white
  when its luma is below 128, else toward black (`#1F1F1F → #686868`,
  `#2D2D2D → #727272`); inactive outlines fade halfway into their fill.

Roles (user-overridable in Tools > Appearance: fill, text, selection; Windows
also border, hover, disabled): `window`, `toolbar` (menu bar and status
strip), `sidebar` (profile list), `label`, `button`, `edit`, `list`,
`checkbox`, `preview`, `status`. Linux check boxes and light buttons take the
full accent as their selection color (the check mark).

## Typography

| | Windows | Linux |
| --- | --- | --- |
| Face | System message font (`NONCLIENTMETRICS.lfMessageFont`, Segoe UI), else Segoe UI | XSETTINGS `Gtk/FontName` family (GNOME/KDE settings manager), else fontconfig `sans` |
| Body | 12 px (9 pt) at 96 DPI | Desktop size, clamped to 11–14 px (forms are authored at 14 px) |
| Heading | Body face, semibold | FLTK bold of the same face |
| Title | 16 px semibold | — |
| Monospace | Cascadia Mono, else Consolas, at body size | FLTK `FL_COURIER` (fontconfig `mono`) |
| Scaling | Windows text size × Options > Text size (100/115/130 %), and DPI | FLTK screen scaling |

Never introduce Segoe UI Variable, Inter, Roboto or other bundled faces.

## Geometry

Windows logical units are 96-DPI pixels scaled by DPI and text size
(`window_base_dpi`, [`dialog_layout.hpp`](../src/editor/dialog_layout.hpp)).

| Metric | Windows | Linux |
| --- | --- | --- |
| Control height | `native_control_height` = 26 | 28 inputs, 30 buttons (fixed FLTK forms) |
| Row pitch | control + 8 (34) | authored per form |
| Margin / gap | 12 / 16 (main), 12 / 8 (dialogs) | 15 outer, 10–15 between groups |
| Section heading pitch | 28 | label above field (`FL_ALIGN_TOP_LEFT`) |
| Outline | 1 px | 1 px |
| Focus | 2 px accent ring; edits and drop-downs a 2 px accent bottom edge | FLTK focus box; focused choice tinted |
| Check box | 16 px square | FLTK down box, square |
| Corner radius | 0 everywhere (windows: `DWMWCP_DONOTROUND`) | 0 everywhere |

Windows main window
([`editor_layout.cpp`](../src/editor/editor_layout.cpp)): sidebar width
`clamp(width / 5, 200, 280)`; preview column `clamp(26 % of width, 280, 440)`,
shown from 1100 px; field cap 480; timing label 84; status strip 28 at the
bottom. Single-line edits center their text vertically (non-client padding),
and row labels share the row height (`DialogLayout::row_label`,
`SS_CENTERIMAGE`).

Linux main window (1080 × 790, [`editor_forms.cpp`](../src/editor/linux/editor_forms.cpp),
`StatusPane` in [`editor_pages.cpp`](../src/editor/linux/editor_pages.cpp)) fits
without scrolling; smaller windows scroll rather than overlap:

| Region | x, y, w, h |
| --- | --- |
| New / Duplicate / Delete | 15 / 90 / 180, 15 (h 30) |
| Profiles list | 15, 70, 240, 300 |
| Toggle saved / Import | y 380; Export JSON / Export pack y 416 |
| Preservation / module faults | 15, 474, 240, 72 |
| Daemon status and capability faults | 15, 572, 240, 142 (11 px) |
| Tabs (Profile … Visual) | 270, 15, 775, 435 |
| Preview | 270, 460, 775, 190 |
| Save / reload outcome | 270, 676, 775, 38 |
| Save / Undo / Redo / Daemon status | y 722; accessibility note 450, 723, 595, 28 |

## Components

- **Button.** Square fill + 1 px outline. Neutral: `panel` fill, `border`
  outline, `button_hover`/`pressed` states. Primary (the dialog's default
  button, one per surface): accent fill and outline, on-accent text. Disabled:
  `divider` outline, `disabled` text. Color buttons show a 16 px square
  swatch inset 8 px with left-aligned text.
- **Edit.** `input` fill, 1 px `border` outline (`divider` when disabled),
  2 px accent bottom edge while focused, text vertically centered.
- **Drop-down list.** Painted like an edit: square, chevron in a 26 px right
  zone, hover/open fill `row_hover`, accent bottom edge when focused or open,
  window fill when disabled. The drop-down list itself keeps the dark theme.
- **Check box / radio.** 16 px square box (radio: circle). Checked: accent
  fill with an on-accent mitred tick (radio: accent dot). Hover lifts the fill
  and brightens the outline.
- **List rows.** `fill` → `row_hover` on hover → selection tint with a 3 px
  accent rail. Profile rows: square check box, name, secondary executable, and
  an "Off" tag (state, never disabled color).
- **Tabs (Linux).** Flat squares; only the selected tab carries the
  selection tint, unselected tabs use `panel`.
- **Menu bar.** `background` strip with a bottom divider; drop-down menus are
  outlined flat boxes (Linux) or the dark system menu (Windows).
- **Status strip (Windows).** `background` with a 1 px `divider` top line and
  secondary text: action outcome left, recognition and LayerD state right.
- **Window frame (Windows 11).** Dark caption, caption and border colors from
  the menu role, square corners.

## Implementation map

| Concern | Windows | Linux |
| --- | --- | --- |
| Tokens and roles | `native_theme.hpp/.cpp` (`neutral_theme`, `native_theme_colors`) | `editor_appearance.hpp/.cpp` (`theme_role_colors`, `control_border`) |
| Fonts | `create_native_font`, `ui_face` | `editor_style.cpp` (`apply_desktop_font`, `parse_desktop_font`, `xsettings_string`) |
| Control painting | `native_controls.cpp` (`paint_native_button`, `paint_native_check_box`, `paint_flat_combo`, `paint_flat_frame`, menu bar) | `editor_style.cpp` (`install_editor_style`: flat box types, menu-bar box) |
| Applying a theme | `editor_theme.cpp`, `EditorWindow` drawing in `editor_draw.cpp` | `appearance_window.cpp` (`apply_editor_appearance`) |
| Metrics | `dialog_layout.hpp` (`native_control_height`, `row_label`), `editor_layout.cpp`, `editor_automation_layout.cpp` | fixed coordinates in `editor_forms.cpp`, `editor_pages.cpp`, dialogs |
| Startup | dialog chrome on create | `main.cpp` calls `install_editor_style()` before any widget |

## Rules for changes

- Change a token or metric at its single source above; never hard-code a
  color, radius or control height in a dialog.
- New controls reuse the painters; new surfaces use `native_control_height`
  and `DialogLayout` (Windows) or the existing form builders (Linux).
- No `RoundRect`, rounded regions or rounded FLTK box types in editor chrome.
  The overlay preview is the only rounded editor drawing, and it follows the
  profile.
- Keep the System theme working on both platforms; it is the escape hatch.
- Accessibility names, tab order and keyboard paths are part of the design:
  restyling must not rename, reorder or hide controls.
- A visual change updates this document (and its version) in the same commit.

## Verification

1. Build: `cmake --build build/vs2026 --config Release --target layere
   layerc_editor_tests layerc_calibration_tests`; Linux through
   `tools/linux-gui/build.sh` (see [`Linux.md`](Linux.md)).
2. Tests: `layerc_editor_tests`, `layerc_calibration_tests`,
   `layerc_linux_editor_model_tests` (tokens, outlines, font parsing) and
   `layerc_linux_editor_widget_tests` under an X display (layout, System
   theme restore, small-window scrolling).
3. Rendered check with cua-driver window screenshots: main window empty and
   populated, Automation, Appearance, Options, Detected Tasks; on Linux the
   main window and Appearance under WSLg or a real X11 desktop. Compare
   against the rules above, not against memory.
   - Windows menus may not open through synthetic clicks; posting the menu
     command (`WM_COMMAND`, ids in `src/editor/editor_resource.h`) to the
     editor window opens dialogs reliably for review.
   - WSLg windows accept cua-driver screenshots but not its input; drive the
     X11 editor with keyboard input from inside the distribution (`xdotool
     windowactivate --sync <id>; xdotool key …`).
4. The XSETTINGS font path needs a desktop with a settings manager (GNOME,
   KDE); WSLg and Xvfb exercise only the fontconfig fallback.

## Revert or reimplement

- **Inspect:** `git show design-v1-square-native` is the complete
  implementation; `git diff design-v0-rounded design-v1-square-native -- src/`
  is the source delta from the previous look.
- **Revert the look:** `git revert design-v1-square-native` restores v0;
  resolve conflicts in the files of the implementation map, then rerun the
  verification above.
- **Reimplement elsewhere** (a new toolkit, branch or rewrite): apply the
  tokens, typography and geometry tables and the component rules; port the
  painters listed in the implementation map; keep the tests named above
  passing, including the Linux helper tests for outlines and font parsing.
- **Evolve:** bump the version here, describe what changed and why, tag the
  commit `design-vN-<name>`, and keep the previous tag for comparison.
