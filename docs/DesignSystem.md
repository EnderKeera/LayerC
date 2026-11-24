# LayerE Design System

The visual language of the LayerC editors: Windows `layere.exe` (Win32/GDI)
and Linux `layere` (FLTK on X11). This document owns the editor look; source
owns exact behavior. Behavior, support claims and evidence stay in
[`README.md`](../README.md) and [`Linux.md`](Linux.md).

**Version:** v2.1 (2026-11-16), a draft on top of v2, "square native,
accessible" (2026-11-07). v2 is in the code at `design-v2-accessible`; the
v2.1 changes below are listed under [Not yet in the
code](#not-yet-in-the-code) until they land.
**Previous look:** v1, tagged `design-v1-square-native`; before it
`design-v0-rounded`. See [Revert or reimplement](#revert-or-reimplement).
**Visual reference:** [`design-system/`](design-system/) holds the tokens,
each component's notes and an HTML preview of every control and screen; open
[`design-system/index.html`](design-system/index.html) in a browser. This
document wins when the two disagree.

**Scope:** editor chrome only — the main window, dialogs, the calibration
window and their controls. Out of scope: the in-game overlay
([`src/daemon/overlay_render.cpp`](../src/daemon/overlay_render.cpp), Linux
overlays), whose shape follows each profile's own `corner_radius`
([Overlay](#overlay) sets only its readability rules); the editor preview of
that overlay (it must stay faithful to the profile); and the Game Bar XAML
widgets.

## Changes from v2

- **Overlay preview:** Add overlay item and Edit overlay item show the item at
  actual size, drawn by the overlay's own painter over a Dark or Bright
  backdrop, with Show for its state, readability lines, an item Color and Add
  item / Save item ([OverlayPreview](#overlaypreview)). The main window's
  preview corner uses the same painter.
- **Lit cards:** text on a lit indicator card follows the `on_accent` rule
  ([Overlay](#overlay)).
- **Keyboard shortcuts** grouped by menu, one shortcut per row, keys as chips
  ([ShortcutsDialog](#shortcutsdialog)).
- **Runtime diagnostics** with status marks, an empty state and Summary / Raw
  JSON ([DiagnosticsDialog](#diagnosticsdialog)).
- **Choose color** names its target, shows current and new in use and checks
  every pair ([ColorPicker](#colorpicker)); **Appearance** lists a role's
  colors with their contrast ([AppearanceRoles](#appearanceroles)).
- **Menus:** one home per command, context menus that mirror the menu bar,
  Text size in View only, unique access keys ([Menus](#menus)).

## Changes from v1

- **Status and information colors:** `yes`, `no`, `warning` and `on_status`,
  and a per-preset `accent_2`, each paired with a drawn glyph or a word; text
  roles `subtext` and `verbose`.
- **Contrast floors per surface** ([Where each color may
  go](#where-each-color-may-go)), Appearance's refusal minimums written down,
  and a live warning line while picking.
- **Named blends:** `primary_hover` and `primary_pressed`; Linux outlines
  blend 91/255 instead of 84/255.
- **New sections:** [Writing](#writing), [Iconography](#iconography),
  [Keyboard](#keyboard), [Screen readers](#screen-readers), [Contrast
  themes](#contrast-themes), [Overlay](#overlay) and [Known
  deviations](#known-deviations-in-the-shipped-editor).
- **One control height** in every dialog, flat frames for bordered statics,
  and one dialog button order: OK · Cancel · Apply.
- **New components:** NumberField, Tooltip, ContextMenu, Scrollbar,
  EmptyState, Dialog and OverlayBar.
- **Main window layout:** a preview corner over three settings slots that
  every overlay kind fills ([Windows main window](#windows-main-window)).
- **Accessibility fixes adopted from the Proposed page:** an overlay contrast
  line (1), disabled buttons that say why (2), a drag hint that names the
  arrow keys (3), a full `accent` outline on focused fields (4), one key
  column in Keyboard shortcuts (7), [Screen readers](#screen-readers) (10),
  [Contrast themes](#contrast-themes) (12), an `accent` bar on the selected
  Linux tab (13), the `accent` focus ring on Linux (14), Linux text that
  follows the desktop size (16) and overlay flashing capped at 3 Hz (17).

## Principles

1. **Square.** Use `radius-none` on every control, focus ring and window
   frame. Only radio buttons take `radius-radio`, because roundness is their
   meaning.
2. **Native type.** Set text in the platform's own interface font at its own
   size. Never bundle or load a face.
3. **Flat.** Solid fills inside a 1 px `outline`; no bevels, gradients or
   shadows, and no Win32 client edge: every frame is a flat 1 px `border`
   (`paint_flat_frame`). Depth comes from the neutral ramp, not effects.
4. **One accent.** `accent` marks state — the primary action, selection, focus
   and checked state — and nothing decorative. The status colors and
   `accent_2` carry meaning in text and glyphs only; they never take over the
   accent's jobs.
5. **One rhythm.** One control height in every window and dialog, and an 8 px
   spacing grid per platform.
6. **Same product on both platforms.** Linux uses the same ramp, presets and
   roles as Windows; toolkit differences stay in the platform adapters.

## Writing

The editor's wording is plain and consistent. Keep it that way:

- **Sentence case** for every label, button, heading and menu item: "Add
  cooldown", "Show advanced", "Visual trigger for Dash".
- **"..." (three periods)** ends a button that opens another window or picker:
  Browse..., Calibrate..., Add..., Edit.... A button that acts at once has
  none: Capture key, Save.
- **Status lines** name the state, then what is happening: "Ready — connecting
  to LayerD.", "Autosaved — LayerD updated live.", "Not autosaved — finish the
  invalid field." A spaced em dash, a period at the end.
- **Errors** say what happened, why, and what to do, in that order, after the
  `no` glyph. The editor writes "Could not", not "Couldn't": "Could not save
  the selected options." is today's form; add the why and the fix after it.
- **Hints** are a fragment or one sentence in `subtext`: "0 = off", "Drag the
  bar in the preview, or nudge it with the arrow keys."
- **One name per thing on both platforms.** The daemon is "LayerD" in
  everything a person reads, including transport-error summaries. Linux uses
  "LayerD status" for the button and "LayerD status and capability faults"
  for the pane.
- **Delete or remove:** "Delete" takes a file off disk (profiles); "Remove"
  takes something out of a profile (cooldowns, panel items).
- **Buttons that close a dialog** name what they do: Add item, Save item.
  "Apply" only keeps a settings dialog open.
- **Reasons** for a disabled button are one sentence naming what holds it:
  "A profile needs at least one cooldown.", "A panel needs at least one
  item." ([Button](#button)).
- **Units** follow a space: "6.5 s", "130 %", "Duration (s)". Keys are written
  as pressed: Ctrl+Shift+S, Up / Down, Alt+letter.
- No exclamation marks, no emoji.

## Color

The themes are the Appearance presets. Every preset shares one neutral dark
ramp (Windows 11 dark values) and differs only in `accent`:

| Theme | `accent` |
| --- | --- |
| Graphite / Copper — the default on both platforms | `preset-copper` #d88e4b |
| Graphite / Teal | `preset-teal` #5ab6aa |
| Graphite / Plum | `preset-plum` #b382e2 |
| Graphite / Slate (`dark` in code) | `preset-slate` #8fa3b8 |

A fifth preset, **System**, uses the Windows accent color on Windows and
FLTK's own palette on Linux, so it has no fixed values here. Keep it working
on both platforms: it is the escape hatch. Its `accent_2` follows the same
hue rule as the presets: violet when the accent's hue lies within 45° of the
blue's (the default Windows blue), else blue.

Pick fills by surface:

- `background` — window, menu bar, status strip, behind labels.
- `input` — edits, lists, the profile sidebar, the overlay preview.
- `panel` — neutral buttons; unselected Linux tabs.
- `button_hover` / `pressed` — neutral button under the pointer / while
  pressed; `button_hover` also for menu-bar items.
- `row_hover` — list rows and drop-downs under the pointer, and an open
  drop-down.
- `selection` — a selected row or tab.

Pick text by role, brightest first: `text` for primary text; `secondary` for
labels, status text, "Off" tags and executable names; `subtext` for hints
beside or under a field; `verbose` for diagnostics and trace lines, always in
the mono face; `disabled` only for disabled text and glyphs. Use `on_accent`
on any `accent` fill. Outline enabled Windows controls in `border`; use
`divider` for disabled outlines and the menu-bar and status-strip lines.

Derived colors are computed from `accent` and the ramp, never hard-coded
anywhere else:

- `selection` = blend(`background`, `accent`, 60/255): a quiet tint that keeps
  `text` above 7:1. List rows add a 3 px `accent` rail (`accent-rail`).
- `on_accent` = #141414 when the accent's luma (299R + 587G + 114B) is at
  least 140000, else #ffffff. All four presets give #141414.
- Primary button: fill `accent`; hover `primary_hover` = blend(`accent`,
  `text`, 40); pressed `primary_pressed` = blend(`accent`, `panel`, 60), the
  button's own fill. A checked check box uses the same hover, and
  blend(`accent`, its fill, 60) while pressed.
- blend(a, b, t) = a + (b − a) × t / 255 per channel, truncated toward zero
  (`blend_color`).
- Linux outlines: `control_border(fill)` blends the fill 91/255 toward white
  when its luma is below 128, else toward black, so every outline clears 3:1
  against its fill — `linux-outline-background` on `background` (the same
  #6e6e6e as Windows' `border`), `linux-outline-panel` on `panel`. A disabled
  control's outline fades halfway into its fill (exempt from the 3:1).

### Status and information

Mark outcomes with three status colors and one secondary color. Each pairs
with a glyph or a word every time: under red–green color blindness the three
statuses turn into similar yellows, and `no` has the same lightness as
Copper's accent, so the shape is what carries the meaning.

| Token | Glyph | For |
| --- | --- | --- |
| `yes` #00e676 | ✓ | Worked or healthy: LayerD running, validation passed, saved |
| `no` #ff5a5a | ✕ | Failed or stopped: LayerD not running, rule errors, capability faults |
| `warning` #ffd740 | ! | Works but needs a look: low overlay contrast, unsaved draft, degraded XWayland |
| `accent_2` (blue on Copper and Plum, violet on Teal and Slate) | … | Neither good nor bad: in progress ("connecting"), notes, links |

- Use them for text, glyphs and small marks (pips, a status square). Never
  fill a button, show focus or selection, or mark the primary action with
  them: that stays `accent`.
- Set the status word semibold, in the body size.
- On a selected row, the message turns `text` and only the glyph keeps its
  status color.
- Put `on_status` (#141414) on any status fill.
- Don't tint backgrounds with a status color: a red tint reads like Copper's
  `selection`.

#### Where each color may go

What the contrast test asserts, in every preset (`layerc_editor_tests`, and
`layerc_linux_editor_model_tests` for the Linux outlines):

| Color | Needs | `background` | `input` | `panel` | `selection` |
| --- | --- | --- | --- | --- | --- |
| `text`, `secondary` | 4.5:1 | ✓ | ✓ | ✓ | ✓ |
| `subtext`, `verbose` | 4.5:1 | ✓ | ✓ | ✓ | not used |
| `yes`, `warning`, `accent_2` | 4.5:1 | ✓ | ✓ | ✓ | ✓ |
| `no` | 4.5:1 | ✓ | ✓ | ✓ | glyph only, 3:1 |
| `accent` as a mark (rail, ring, edge, tick fill) | 3:1 | ✓ | ✓ | ✓ | ✓ |
| `border`, Linux outlines | 3:1 against their own fill | ✓ | ✓ | edge against `background` | not used |
| `on_accent` on `accent`, `on_status` on a status fill | 4.5:1 | | | | |
| `disabled` | 3:1 against its role's fill (Appearance's floor) | ✓ | ✓ | ✓ | |

### Roles

People can override each role's fill, text and selection in Tools > Appearance
(Windows also border, hover and disabled). The roles: `window`, `toolbar`
(menu bar and status strip), `sidebar` (profile list), `label`, `button`,
`edit`, `list`, `checkbox`, `preview`, `status`. Linux check boxes and light
buttons take the full `accent` as their selection color (the check mark). Read
colors through the role, so an override reaches every control. On Windows,
Appearance checks every role's overrides when you press Apply or OK and refuses to save
below these minimums, naming the role, the pair and the ratio: Text / Fill
4.5:1, Disabled / Fill 3:1, Text / Hover 4.5:1 (buttons and check boxes), Text
/ Selection 4.5:1 (buttons, check boxes, lists and the profile list). The
FLTK editor checks the same floors where it paints the text: on the role's
own fill when its control draws one, otherwise on the surface beneath (a
label or check box on the window, a tab label on its tab), and names that
surface: "Labels: Text / Fill contrast is 1.3:1 on the Window fill; at least
4.5:1 is required." On Windows it also checks the record it shares with the
Win32 editor, including the hover and disabled colors it keeps but cannot
edit; border contrast remains advisory. It checks that merged record again
before writing. Primary actions must also clear 4.5:1 on the accent and
their painted hover and pressed fills. This uses each painter's actual ink,
so a custom accent can be refused without changing the preset token rule.
Appearance checks open tool windows and both Diagnostics text modes. It also
checks the fixed ink a control paints on its fill or the fill beneath it:
empty-list titles and hints (including the dimmed list fill with no profile
open), disabled-button reasons and Options hints. These inks ignore Text
overrides; System checks them only when that fill is overridden. The
positioning caption uses Preview text. In System, an untouched desktop pair
never refuses, but each pair that uses an overridden color is checked. The
refusal never comes as a surprise:

- **While picking,** Choose color shows one line under its preview with the
  pair nearest its minimum (the lowest ratio over minimum among the colors
  the role measures it against) and that color: "Contrast with Window text:
  12.9:1". Below that pair's minimum the
  line leads with the `warning` glyph and names the minimum: "! Contrast with
  Window text: 3.2:1; Appearance needs 4.5:1." A Border color is measured
  against its role's fill and warns below 3:1, without a refusal.
- **In Appearance,** while any role is below a minimum, a `warning` line
  above the buttons (under the role list while advanced colors show) names
  the first failing role, pair and ratio, the same words the refusal uses.
  The line keeps its row while empty, so the dialog does not jump. A draft
  that leaves the line's own text below 4.5:1 cannot hide it: the message
  then takes the better of `text` and `on_status` on its fill.
- Choose color opened from the main window's overlay color buttons measures
  against the bar's colors instead ([OverlayBar](#overlaybar)).

### Token values

Every value, from the design system's `tokens.json` (also in
[`design-system/`](design-system/tokens.json)).

| Token | Value | Use |
| --- | --- | --- |
| `background` | #1f1f1f | Window, menu bar, status strip and the fill behind labels. The ground for `text`, `secondary` and `accent`. |
| `panel` | #2d2d2d | Neutral button fill; unselected Linux tabs. `text` and `secondary` read on it. |
| `input` | #171717 | Edits, lists, the profile sidebar and the overlay preview's ground. |
| `border` | #6e6e6e | 1 px outline of enabled Windows controls: 3.2:1 on `background`, 3.5:1 on `input`. On a `panel` button it reads 2.7:1 against its own fill; the button's edge against `background` carries the 3:1. |
| `divider` | #333333 | Disabled control outlines and the menu-bar and status-strip lines. Decorative only (1.3:1 on `background`): never the only edge of something that can be clicked. |
| `text` | #f3f3f3 | Primary text on `background`, `panel`, `input`, `button_hover`, `row_hover`, `pressed` and `selection` (at least 9.7:1 on each). |
| `secondary` | #b8b8b8 | Labels, status text, "Off" tags and executable names, on `background`, `input`, `panel` and `selection` (at least 5.4:1). Above `subtext` and `verbose` in the text ladder. |
| `subtext` | #a3a3a3 | Hints beside or under a field ("0 = off", the drag hint, why a button is disabled): 6.5:1 on `background`, 7.1:1 on `input`, 5.5:1 on `panel`. Not on selected rows (4.3:1 at worst); never smaller than the body size. |
| `verbose` | #9a9a9a | Diagnostics and trace lines, always in `win-mono` / `linux-mono` so the face sets them apart from `subtext`: 5.9:1 on `background`, 6.4:1 on `input`, 4.9:1 on `panel`. Not on selected rows. |
| `disabled` | #808080 | Disabled text and glyphs only. Appearance refuses an override below 3:1 against the role's fill; the defaults give 4.2:1 on `background` and 3.5:1 on `panel`. |
| `button_hover` | #383838 | Neutral button, menu-bar item and check box fill under the pointer (a check box's outline turns `text` too). |
| `row_hover` | #2a2a2a | List row, drop-down and edit-role hover fill, and an open drop-down. |
| `pressed` | #262626 | Neutral button fill while pressed. |
| `yes` | #00e676 | Worked or healthy (LayerD running, validation passed, saved), always with a ✓ or a word: 9.9:1 on `background`, 10.7:1 on `input`, 8.3:1 on `panel`, 6.4:1 or more on `selection`. Text and glyphs only, never a control fill. |
| `no` | #ff5a5a | Failed or stopped (LayerD not running, rule errors, capability faults), always with a ✕ or a word: 5.4:1 on `background`, 5.9:1 on `input`, 4.5:1 on `panel`. On a selected row (3.5:1 at worst) only the glyph keeps this color and the message turns `text`. Same lightness as Copper's accent, so never rely on hue. |
| `warning` | #ffd740 | Works but needs a look (low overlay contrast, unsaved draft, degraded XWayland), always with a ! or a word: 11.8:1 on `background`, 12.8:1 on `input`, 9.9:1 on `panel`, 7.7:1 or more on `selection`. |
| `on_status` | #141414 | Text on a `yes`, `no` or `warning` fill (a badge or pip): 6.0:1 on `no`, 11.0:1 on `yes`, 13.2:1 on `warning`. Fixed rather than the `on_accent` luma rule, which would put white on `no` (3.1:1). |
| `preset-copper` | #d88e4b | Graphite / Copper's accent as a fixed value, for showing presets side by side (Appearance swatches, the cover). In UI, use `accent`. The default preset on both platforms. |
| `preset-teal` | #5ab6aa | Graphite / Teal's accent as a fixed value, for showing presets side by side (Appearance swatches, the cover). In UI, use `accent`. |
| `preset-plum` | #b382e2 | Graphite / Plum's accent as a fixed value, for showing presets side by side (Appearance swatches, the cover). In UI, use `accent`. |
| `preset-slate` | #8fa3b8 | Graphite / Slate's accent as a fixed value, for showing presets side by side (Appearance swatches, the cover). In UI, use `accent`. Called `dark` in code. |
| `linux-outline-background` | #6e6e6e | Linux outline on a `background` fill: `control_border` blends the fill 91/255 toward white, which lands exactly on Windows' `border`. 3.2:1 on `background`. A disabled control's outline fades halfway into its fill and needs no minimum. |
| `linux-outline-panel` | #777777 | Linux outline on a `panel` fill (buttons, unselected tabs): 3.1:1 on `panel` at 91/255. An edit's outline on `input` is #696969, 3.3:1. |

| Token | Copper | Teal | Plum | Slate | Use |
| --- | --- | --- | --- | --- | --- |
| `accent` | #d88e4b | #5ab6aa | #b382e2 | #8fa3b8 | The one accent: primary button fill, selection rail, focus ring and edge, checked boxes. Never decorative. At least 4.7:1 on `background`, `input` and `panel` in every preset. |
| `on_accent` | #141414 | #141414 | #141414 | #141414 | Text and the check tick on an `accent` fill: #141414 when the accent's luma (299R + 587G + 114B) is at least 140000, else white. Every preset lands on #141414 (6.3:1 or more). |
| `selection` | #4a3929 | #2c423f | #41364c | #393e43 | Selected list row, selected Linux tab: `background` blended 60/255 toward `accent`. `text` reads 9.7:1 or more on it; list rows add a 3 px `accent` rail. |
| `primary_hover` | #dc9d65 | #72bfb5 | #bd93e4 | #9eafc1 | Primary button, and a checked check box, under the pointer: `accent` blended 40/255 toward `text`. `on_accent` stays the label. |
| `primary_pressed` | #b07844 | #50968d | #946eb8 | #788898 | Primary button while pressed: `accent` blended 60/255 toward the button's fill (`panel`), as `paint_native_button` does. `on_accent` stays the label, 4.6:1 or more. |
| `accent_2` | #60b0ff | #b99bff | #60b0ff | #b99bff | The secondary color: information that is neither good nor bad (in progress such as "connecting", notes, links). Blue in the Copper and Plum presets, violet in Teal and Slate, so its hue never matches the accent. 7.2:1 on `background`, 6.0:1 on `panel`, 4.7:1 or more on `selection`. Text and glyphs only; it never marks focus, selection or the primary action. |

## Typography

Use the platform's interface face. Never introduce Segoe UI Variable, Inter,
Roboto or any other bundled face.

- **Windows** — the system message font (`NONCLIENTMETRICS.lfMessageFont`,
  normally Segoe UI), else Segoe UI. Set everything in `win-body` (12 px, 9 pt
  at 96 DPI); section headings in `win-heading` (the body face, semibold);
  titles in `win-title` (16 px semibold); paths and code in `win-mono`
  (Cascadia Mono, else Consolas, at body size). Sizes scale with DPI, the
  Windows text size and View > Text size (100 / 115 / 130 %).
- **Linux** — the XSETTINGS `Gtk/FontName` family (from a GNOME or KDE
  settings manager), else fontconfig `sans`, at the desktop size. Forms are
  authored at 14 px: `linux-body`, `linux-heading` (FLTK bold of the same
  face), `linux-mono` (`FL_COURIER`, fontconfig `mono`); every label uses
  them, the LayerD status pane included. A desktop size of 11 to 14 px sets
  the text size directly, and anything smaller stays 11 px. Above 14 px the
  text stays 14 px and FLTK's screen scale grows by size / 14, so the fixed
  layout grows with the text instead of clipping it. FLTK's own screen
  scaling multiplies in.

| Style | Size | Weight | Use |
| --- | --- | --- | --- |
| `win-title` | 16px | 600 | Window and page titles. |
| `win-heading` | 12px | 600 | Section headings: the body face, semibold. |
| `win-body` | 12px | 400 | All other text: 9 pt at 96 DPI. |
| `win-mono` | 12px | 400 | Monospace at body size: Cascadia Mono, else Consolas. |
| `linux-heading` | 14px | 700 | Headings: FLTK bold of the same face. |
| `linux-body` | 14px | 400 | Body text at the authored size, the LayerD status pane included. A desktop size of 11–13 px brings it down; above 14 px the screen scale grows instead. |
| `linux-mono` | 14px | 400 | FL_COURIER (fontconfig mono) at body size. |

## Iconography

LayerE has no icon set and loads no icon font. Controls draw their own marks
(the check tick, the drop-down chevron), and the four status glyphs are drawn
the same way: on a 12 px grid with 2 px square strokes, painted in their
token's color, never taken from a font, so they look the same on Windows and
Linux. The SVGs are in
[`design-system/assets/Glyphs/`](design-system/assets/Glyphs/): `yes` ✓, `no`
✕, `warning` !, `accent_2` …. Pair each with its word; never use emoji.

The app icons ([`design-system/assets/Logos/`](design-system/assets/Logos/))
are a cooldown ring: a `preset-copper` arc over a #3e444e track on a #1e232a
rounded square. The editor's icon adds a #50bac8 pencil and the calibration
helper's a crosshair; at 16 px all three are the bare ring. Copy the files;
never redraw the mark.

## Geometry

Windows values are 96-DPI logical pixels, scaled by DPI and text size
(`window_base_dpi`).

- **Controls.** Every Windows button and edit is `win-control-height` (26) in
  every window and dialog, Appearance and Choose color included; drop-downs
  keep the native combo box height, `win-combo-height` (23). A disabled edit
  keeps the `input` fill so it still reads as a field, as the code intends
  (`editor_draw.cpp`); only its outline turns `divider` and its text
  `disabled`. Linux forms are fixed: edits and choices `linux-input-height`
  (28), buttons `linux-button-height` (30). Outlines are 1 px on both.
- **Rows.** Windows rows step by `win-row-pitch` (control + 8 = 34); a section
  heading takes `win-heading-pitch` (28). Linux places each label above its
  field (`FL_ALIGN_TOP_LEFT`).
- **Spacing.** Windows main window: `win-margin` (12), `win-gap-main` (16)
  between columns, `win-section-gap` (12) between groups in the profile
  column. Windows dialogs: `win-margin` (12) and `win-gap-dialog` (8). Linux:
  `linux-margin` (15) outside, 10–15 px between groups (`linux-group-gap` to
  `linux-margin`).
- **Focus.** One signal on both platforms: a 2 px ring (`focus-width`) inside
  the focused control's edge, `accent` everywhere except on the primary
  button, where it is `text`. On an edit or drop-down the ring replaces the
  1 px outline, so the focused field is one 2 px `accent` frame on every
  surface; on a check box it wraps the box and its label. A focused list
  takes a 2 px `accent` bottom edge and rings its focused row, so two rings
  never stack. Linux draws the same ring in place of FLTK's dotted focus box
  on every focusable widget. The ring is the whole signal: no fill or tint
  changes on focus.
- **List rows** are `win-list-row` (27) in item lists and `win-profile-row`
  (21) in the profile list, inset 1 px inside the list's outline.
- **Check boxes** are 16 px squares (`checkbox`); FLTK uses its down box,
  square.
- **Corners** are `radius-none` everywhere. Windows windows set
  `DWMWCP_DONOTROUND`.

### Windows main window

The profile sidebar is clamp(width / 5, `win-sidebar-min`, `win-sidebar-max`).
Right of it are two bands; the [Layout
page](design-system/components/MainWindowLayout/preview.html) draws them.

- **Top band:** the profile title and fields, the item list with its buttons,
  and the **preview corner** on the right. The list and the preview end on one
  line.
- **Settings row:** three equal slots across the full width, `win-gap-main`
  apart, anchored to the status strip. Slot 1 is what drives the item (Timing
  and key for a cooldown), slot 2 how LayerC sees it start (Visual trigger),
  slot 3 its look (Overlay bar). The row is as tall as its tallest group, and
  the list and preview take the rest, so Show advanced adds rows without
  growing the window; the list keeps at least 84 px.
- **The preview corner** is as wide as slot 3 and sits right over it; it
  frames the target at its own shape (1920 × 1080 without a live game) with
  the Actual size bar under the frame.
- **Slots keep their place.** A kind with no group for a slot leaves it empty,
  so the look stays under the preview. Each overlay kind fills the same slots:
  [Overlay](#overlay) has the kinds, the [Layout
  page](design-system/components/MainWindowLayout/preview.html) the table.
- **Narrow windows.** Three slots need `win-slot-min` (260) each. Below
  `win-preview-from` (1100 px), or when they don't fit, the preview hides,
  slots 1 and 2 stack in the first column, and Position on window moves into
  the look slot. Changing text size grows a retained narrow window to the
  same scaled minimum extent as interactive resizing, capped to its monitor's
  work area. When stacked rows would exceed the usable height, the minimum
  width keeps the three slots side by side instead of clipping bottom rows.
- **Tab order** follows reading order: fields, list, preview corner, then
  slots 1 to 3.
- **The look slot** ends with the drag hint in `subtext` ("Drag the bar in
  the preview, or nudge it with the arrow keys. Direct drag works during
  Position on window.") and the overlay contrast line
  ([OverlayBar](#overlaybar)).
- **Reasons** for disabled buttons sit under their buttons in `subtext`, such
  as "A profile needs at least one cooldown." under Remove cooldown
  ([Button](#button)).

Fields stop at `win-field-cap` (480); the timing label is `win-timing-label`
(84); the status strip is `win-status-strip` (28) at the bottom. Single-line
edits center their text vertically, and row labels share the row height
(`DialogLayout::row_label`, `SS_CENTERIMAGE`).

### Linux main window

1080 × 790 (`linux-window-width` × `linux-window-height`), fitting without
scrolling; smaller windows scroll rather than overlap.

| Region | x, y, w, h |
| --- | --- |
| New / Duplicate / Delete | 15 / 90 / 180, 15 (h 30) |
| Profiles list | 15, 70, 240, 300 |
| Toggle saved / Import | y 380; Export JSON / Export pack y 416 |
| Preservation / module faults | 15, 474, 240, 72 |
| LayerD status and capability faults | 15, 572, 240, 142 (body size) |
| Tabs (Profile … Visual) | 270, 15, 775, 435 |
| Preview | 270, 460, 775, 190 |
| Save / reload outcome | 270, 676, 775, 38 |
| Save / Undo / Redo / LayerD status | y 722; accessibility note 450, 723, 595, 28 |

The three status panes reserve 18 px inside their authored rectangles for the
decorative glyph; their text outputs use the remaining width. Accessible values
and announcements carry the full message, not a duplicate glyph character.

### Size and spacing tokens

| Token | Value | Use |
| --- | --- | --- |
| `outline` | 1px | Every control outline, on both platforms. |
| `focus-width` | 2px | 2 px focus on both platforms (`paint_focus_ring`): an `accent` ring inside the edge of the focused button, edit, drop-down, list row, tab or check box (around box and label); on the primary button the ring is `text`. On edits and drop-downs it replaces the 1 px outline. A focused list takes a 2 px `accent` bottom edge and rings its focused row. |
| `accent-rail` | 3px | The `accent` rail on a selected list row, and the bar along the top of the selected Linux tab. |
| `checkbox` | 16px | Check box and radio size (`native_check_box_size`); the label starts 8 px after it, and the whole width is the click area. |
| `swatch` | 16px | The square swatch in a color button. |
| `win-control-height` | 26px | Windows native_control_height: every button and edit. Drop-downs are `win-combo-height`. In code: `native_control_height` (`dialog_layout.hpp`). |
| `win-combo-height` | 23px | Windows drop-downs: a drop-down list's height is set by Windows from the font, so layout code can't change it; 23 px with the 12 px body font. |
| `win-row-pitch` | 34px | Windows: one form row (control height + 8). In code: `DialogLayout` (`dialog_layout.hpp`). |
| `win-list-row` | 27px | Windows: one row in an item list (cooldowns, panel items, control roles). Measured from screenshots; not named in the source document. |
| `win-profile-row` | 21px | Windows: one row in the profile list. Measured from screenshots; not named in the source document. |
| `win-heading-pitch` | 28px | Windows: a section heading's row. In code: `DialogLayout` (`dialog_layout.hpp`). |
| `win-chevron-zone` | 26px | Windows: the drop-down's chevron zone at its right. In code: `paint_flat_combo` (`native_controls.cpp`). |
| `win-status-strip` | 28px | Windows: status strip height at the bottom of the main window. In code: `editor_layout.cpp`. |
| `win-sidebar-min` | 200px | Windows: profile sidebar width is clamp(width / 5, 200, 280). In code: `editor_layout.cpp`. |
| `win-sidebar-max` | 280px | Windows: the sidebar's widest. In code: `editor_layout.cpp`. |
| `win-preview-from` | 1100px | Windows: the preview corner shows from this window width, as wide as slot 3; below it the settings slots stack. In code: `editor_layout.cpp`. |
| `win-slot-min` | 260px | Windows main window: the narrowest a settings slot gets; below it slots 1 and 2 stack in one column. See the Layout page. |
| `win-field-cap` | 480px | Windows: the widest a field grows. In code: `editor_layout.cpp`. |
| `win-timing-label` | 84px | Windows: the timing label's width. In code: `editor_layout.cpp`. |
| `linux-input-height` | 28px | Linux: edits and choices. Single source: `editor_input_height` in `editor_style.hpp`; forms and dialogs reuse it. |
| `linux-button-height` | 30px | Linux: buttons. Single source: `editor_button_height` in `editor_style.hpp`; forms and dialogs reuse it. |
| `linux-sidebar` | 240px | Linux: profiles list and sidebar panes. In code: `editor_forms.cpp`. |
| `linux-window-width` | 1080px | Linux main window width; it fits without scrolling. In code: `editor_forms.cpp`. |
| `linux-window-height` | 790px | Linux main window height; smaller windows scroll rather than overlap. In code: `editor_forms.cpp`. |
| `win-gap-dialog` | 8px | Windows: between controls in dialogs, and the gap that makes the row pitch (control + 8). In code: `DialogLayout` and `editor_layout.cpp`. |
| `win-margin` | 12px | Windows: main window and dialog margin. In code: `DialogLayout` and `editor_layout.cpp`. |
| `win-gap-main` | 16px | Windows: between groups in the main window. In code: `DialogLayout` and `editor_layout.cpp`. |
| `win-section-gap` | 12px | Windows: between groups in the profile column (Timing and key, Overlay bar, Visual trigger). In code: `section_gap` (`editor_layout.cpp`). |
| `swatch-inset` | 8px | Inset of the 16 px swatch inside a color button. |
| `linux-group-gap` | 10px | Linux: the smallest gap between groups; forms use 10–15 px. In code: `editor_group_gap` (`editor_style.hpp`). |
| `linux-margin` | 15px | Linux: outer margin of the main window and dialogs, and the largest gap between groups. |
| `radius-none` | 0px | Every control, focus ring and window frame (Windows sets DWMWCP_DONOTROUND). |
| `radius-radio` | 50% | Radio buttons and their dot, nothing else. |

## Keyboard

On Windows, field labels and buttons carry an access key (the n of Profile
name, the B of Browse..., the S of Show text), and Alt+letter moves to it. The underline
follows Windows' keyboard cues: hidden until Alt is pressed, unless "Underline
access keys" is on (`ODS_NOACCEL`, `UISF_HIDEACCEL`). Give each new control a
letter that is unique on its surface. The Windows editor's shortcuts are below;
Linux keeps its command registry's bindings, shown by F1.

| Keys | Action |
| --- | --- |
| Ctrl+N | New profile |
| Ctrl+S / Ctrl+Shift+S | Save / Save as |
| Ctrl+Z / Ctrl+Y | Undo / Redo |
| Ctrl+D | Duplicate the selected cooldown |
| Ctrl+Shift+A | Show or hide advanced settings |
| Ctrl+, | Options |
| F2 | Rename the selected profile or cooldown |
| Del | Delete the selected profile, or remove the selected cooldown |
| Shift+F10 | Context menu |
| Up / Down, mouse wheel | Step the focused number: Duration 0.1 s (Shift: 1 s), bar width and height 1 px |
| Arrow keys | Nudge the selected bar in the preview 1 px (Shift: 10 px) |
| Alt+letter | Jump to the underlined field |
| F1 | Keyboard shortcuts (Help menu) |

Help > Keyboard shortcuts (F1) groups the shortcuts by menu, one per row, with
each key drawn as a square chip in a column as wide as its longest entry and
the action in `text` beside it, F1 included
([ShortcutsDialog](#shortcutsdialog)). On Windows it is a [Dialog](#dialog)
with a primary OK, not a message box; Linux shows the same groups. Both build
the list from the editor's command table, so a new shortcut appears in it
without an edit.

## Screen readers

What a sighted person reads, a screen reader says: UI Automation on Windows
(Narrator, NVDA), AT-SPI on Linux (Orca, through `EditorAccessibility`'s
own D-Bus adaptor).

- **Names** are the visible label without its access-key marker ("Profile
  name", "Browse..."). A button without a word (+, −, ...) takes its
  tooltip's words: "New profile", "Delete profile", "More profile actions".
- **Color buttons** add their color to the name: "Fill, color #DDBC17".
  Choose color's palette swatches carry their name and position ("Copper
  #D88E4B, 7 of 24") and the arrow keys move between them.
- **Descriptions:** the hint, reason or warning line that belongs to a
  control, and its tooltip, are its accessible description, so the reader
  says "Active (ms), edit, 0. 0 = off". That covers "0 = off", a disabled
  button's reason, the drag hint and the contrast lines.
- **Announcements:** the outcome of an explicit action (Save, Save as,
  reload, import, export) and every failure, autosave included, are
  announced as they appear in the status line: a UI Automation notification
  on Windows, an AT-SPI announcement on Linux. A successful autosave stays
  silent, since it follows every edit.
- **Order:** a screen reader walks the tab order ([Keyboard](#keyboard)), and
  no information exists only as color, position or a tooltip.

## Contrast themes

With a Windows contrast theme on (`SPI_GETHIGHCONTRAST` reports
`HCF_HIGHCONTRASTON`), the editor sets the preset and Appearance's overrides
aside and paints every role in the theme's system colors, then repaints on
`WM_SYSCOLORCHANGE` and `WM_SETTINGCHANGE`. The overrides stay saved for when
the theme is off.

| Tokens | System color |
| --- | --- |
| `background`, `input` | `COLOR_WINDOW` |
| `panel` (buttons) | `COLOR_BTNFACE` |
| `text`, `secondary`, `subtext`, `verbose`, `border`, and the status colors | `COLOR_WINDOWTEXT` (`COLOR_BTNTEXT` on a button) |
| `disabled` and `divider` (disabled outlines, menu-bar and status-strip lines) | `COLOR_GRAYTEXT` |
| `button_hover`, `row_hover`, `pressed`, and `on_status` | The role's own fill |
| `accent`, `selection`, the focus ring, hover and pressed outlines | `COLOR_HIGHLIGHT` |
| `on_accent`, text on a selected row, and the focus ring on a `COLOR_HIGHLIGHT` fill | `COLOR_HIGHLIGHTTEXT` |

- The primary button, a checked check box and a selected row fill with
  `COLOR_HIGHLIGHT`. Hover and pressed keep the fill and outline in
  `COLOR_HIGHLIGHT`.
- Status glyphs and words carry their meaning alone, as they already must.
- Appearance says "A contrast theme is on, so Windows colors are in use." and
  keeps its controls working.
- The overlay preview stays faithful to the profile.
- The contrast floors are the theme's to meet, so the floor tests run on the
  presets only. The contrast-theme path is tested through a seam that
  reports the theme on, never by changing the system setting.

Linux has no contrast-theme signal; the System preset takes FLTK's palette,
which follows the X resources.

## Components

Each component below carries its rules: Button, Edit, NumberField, DropDown,
CheckBox (and radio), ListRow, EmptyState, Scrollbar, Tabs (Linux), MenuBar,
ContextMenu, Tooltip, StatusStrip, StatusText, Dialog and WindowFrame (Windows
11), and from v2.1 [OverlayPreview](#overlaypreview),
[ShortcutsDialog](#shortcutsdialog), [DiagnosticsDialog](#diagnosticsdialog),
[ColorPicker](#colorpicker), [AppearanceRoles](#appearanceroles) and
[Menus](#menus). One primary button per surface; Dialog has the button order
for each kind of dialog: OK · Cancel · Apply, the Windows order Options
already uses. New controls reuse the existing painters; new surfaces use
`native_control_height` and `DialogLayout` (Windows) or the existing form
builders (Linux).

The **Screens** pages in [`design-system/`](design-system/index.html) rebuild
four pre-refit Windows surfaces from 96 DPI screenshots, pixel for pixel, out
of these components: the main window with a profile open, Automation, Keyboard
shortcuts, and Appearance with Choose color open. The original screenshots are
in [`design-system/assets/Screenshots/`](design-system/assets/Screenshots/).
Those captures are historical visual references, not the v2 acceptance
baseline. Match the rules above and the Layout and adopted Proposed fixes
instead. A fifth page draws the Linux main window from the documented
coordinates and labels. The Screens pages still show the pre-v2 editor; new
screenshots will replace them.

The [**Layout** page](design-system/components/MainWindowLayout/preview.html)
draws the main window as it should be laid out: the preview corner over three
settings slots, the grid at half size, and how each overlay kind fills the
slots. The Screens page preserves the earlier layout; v2 main-window work
follows the Layout page.

The [**Proposed** page](design-system/components/ProposedFixes/preview.html)
applies 17 accessibility fixes to those screens, with before-and-after
examples of the rest. Fourteen are adopted and now part of the rules in this
document: 1 the overlay contrast line, 2 disabled buttons that say why, 3 the
drag hint, 4 the focus ring on fields, 7 one key column, 8 flat frames, 9 one
control height, 10 screen readers, 12 contrast themes, 13 the selected-tab
bar, 14 the Linux focus ring, 15 Linux outlines, 16 Linux text size and 17
the flash cap. 5 (access keys in Automation) and 11 (the check box's click
area) were in the code already; 6 (disabled fields) was withdrawn, since the
code keeps the input fill on purpose.

The [Overlay](#overlay) section sets the rules for the in-game overlay, which
follows each profile rather than these tokens.

### AppearanceRoles

Tools > Appearance with advanced colors shown. A settings Dialog: Reset all at
the left; OK · Cancel · Apply at the right.

- **Preset** drop-down at the top left; Show / Hide advanced colors at the
  right.
- **Preview:** a small editor drawn in the current colors: menu bar, profile
  list with a selected row, a labelled field, primary, neutral and disabled
  buttons, a checked box and a status strip with a mark. Clicking a part
  selects its role.
- **Roles and colors:** the Control role list (168 px) at the left; at the
  right, "<Role> colors" with one row per color part the role has (Fill, Text,
  Border, Hover, Selection, Disabled): its label, a color button with swatch
  and hex (132 px), and its contrast as StatusText ("✓ 14.8:1 on fill"). Parts
  a role doesn't have aren't shown. A part that differs from the preset adds
  `accent_2` "Changed". Reset selected sits under the rows.
- **Floors** as in Roles: Text / Fill 4.5:1, Disabled / Fill 3:1, Text / Hover
  4.5:1, Text / Selection 4.5:1. Apply and OK refuse below them, naming the
  role, the pair and the ratio; a contrast theme sets the overrides aside.

The consumer supplies the preset, the roles and their overrides.

Preview:
[`components/AppearanceRoles/preview.html`](design-system/components/AppearanceRoles/preview.html).

### Button

A square fill inside a 1 px outline, label centered; at most one primary per
surface.

- **Neutral:** `panel` fill, `border` outline, `text` label; `button_hover`
  under the pointer, `pressed` while pressed.
- **Primary:** the dialog's default button (`DM_GETDEFID`), one per surface
  (OK, Close). `accent` fill and outline, `on_accent` label; `primary_hover`
  under the pointer, `primary_pressed` while pressed.
- **Disabled:** `panel` fill kept, `divider` outline, `disabled` label.
- **Why disabled:** when the reason isn't visible nearby, a one-sentence
  `subtext` line under the button says what holds it ("A profile needs at least one
  cooldown.", "A panel needs at least one item."), and the same
  sentence is the button's tooltip and accessible description. The line goes
  when the button is enabled. Never only a tooltip: a disabled control may
  not show one.
- **Color button:** a `swatch` (16 px, with its own 1 px `border` outline)
  inset `swatch-inset` (8 px) from the left edge, then 8 px, then the
  left-aligned text (Fill, Background, Border, Text).
- **Focus:** a 2 px ring inside the edge (`paint_focus_ring`): `accent` on
  neutral buttons, `text` on the primary one.
- **Access key:** one letter (`&Browse...`), underlined when Windows shows
  keyboard cues (after Alt, or always with "Underline access keys" on).
- **Size:** `win-control-height` on Windows; `linux-button-height` on Linux,
  outlined in `linux-outline-panel`.

The consumer supplies the label with its access key, whether it is the
surface's default action, its enabled state and, for a color button, the
color. Real labels: Show advanced, Browse..., Choose running..., Add cooldown,
Remove cooldown, Capture key, Calibrate..., Add..., Edit..., Remove, Move up,
Move down, Position on window, Validate, Close.

Painted by `paint_native_button` (Windows) and the flat box types of
`install_editor_style` (Linux).

The preview is a static HTML rendition of these rules; the painters named
above are the source of truth. The Screens pages show it in place.

Preview:
[`components/Button/preview.html`](design-system/components/Button/preview.html).

### CheckBox

A 16 px square box (`checkbox`) with its label 8 px after it; the whole
control, label included, is the click area. Labels that cannot fit on one
line wrap, and the control grows to show every line; the box and focus-ring
metrics stay unchanged.

- **Unchecked:** `input` fill, `border` outline. Under the pointer the fill
  turns `button_hover` and the outline `text`; pressed, the fill is `pressed`.
- **Checked:** `accent` fill and outline with an `on_accent` tick: a 2 px
  mitred line through (4, 9), (7, 12) and (12, 5) in the 16 px box. Under the
  pointer `primary_hover`; pressed, `accent` blended 60/255 toward the check
  box's own fill.
- **Disabled:** the role's fill (`background`), `divider` outline, `disabled`
  label; a checked one fills with `divider` and draws its tick in `disabled`.
- **Focus:** a 2 px `accent` ring around the box and its label, on both
  platforms.
- **Radio:** a 16 px circle with an 8 px `accent` dot; disabled, the dot turns
  `disabled`. Runtime diagnostics' Summary / Raw JSON pair uses it
  ([DiagnosticsDialog](#diagnosticsdialog)).
- **Linux:** the FLTK down box, square; the check mark takes the full `accent`
  as its selection color.

The consumer supplies the label with its access key (&Show text), the checked
state and the enabled state. Painted by `paint_native_check_box` through
`NM_CUSTOMDRAW` (Windows).

The preview is a static HTML rendition of these rules; the painters named
above are the source of truth. The Screens pages show it in place.

Preview:
[`components/CheckBox/preview.html`](design-system/components/CheckBox/preview.html).

### ColorPicker

Choose color, opened from a color button in Appearance, the main window or Add
overlay item. A Dialog with OK · Cancel.

- **Target line** at the top: what is being picked, semibold, then where in
  `secondary`: "Window fill · Graphite / Copper", "Fill · panel iwcs".
- **Palette:** 24 swatches, 8 × 3, 44 × 26 px, 6 px apart, each with a 1 px
  `border`. The chosen swatch takes a 2 px `accent` ring 1 px outside it.
  Arrow keys move between swatches, and each is named for screen readers
  ("Copper #D88E4B, 7 of 24").
- **Custom color:** Red, Green and Blue as NumberFields (0–255, Up and Down
  step) and Hex in the mono face, with the hint "0 to 255 each, or #RRGGBB. Up
  and Down step a value."
- **Current and New:** two 200 × 44 samples side by side showing the role's
  real text on its fill, before and after.
- **Contrast:** one StatusText line for each pair the color takes part in,
  with its ratio and floor: "✓ Text 12.4:1 needs 4.5:1". Below an editor floor
  the line is `warning` and ends "Appearance won't save it."; for an overlay
  color the floors are the Overlay rules and the line only warns.

The consumer supplies the target, its current color and the pairs to check.

Preview:
[`components/ColorPicker/preview.html`](design-system/components/ColorPicker/preview.html).

### ContextMenu

The menu opened by right-click or Shift+F10 on a list row: a flat box with a 1
px `border` outline on `background` (Linux), or the dark system menu
(Windows), with each item's shortcut right-aligned in `secondary`.

- Items are 24 px tall, `button_hover` under the pointer, `disabled` when
  unavailable; a 1 px `divider` separates groups.
- Offer the menu bar's group for the row, in the same order, with the same
  verbs and shortcuts but without the noun, because the row is the object:
  Rename (F2), Duplicate (Ctrl+D), Remove... (Del). Menus lists every context
  menu.
- F2 and Del act on the focused list, so they appear here rather than in the
  menu bar.
- Every item has a shortcut or access key, and the menu closes with Esc.

The consumer supplies the items, their shortcuts and enabled states. The items
shown are an example.

The preview is a static HTML rendition of these rules.

Preview:
[`components/ContextMenu/preview.html`](design-system/components/ContextMenu/preview.html).

### DiagnosticsDialog

Tools > Runtime diagnostics: a resizable tool window with a primary Close.

- **Intro** in `subtext`: "LayerD’s health and a one-time snapshot. Nothing is
  kept after this window closes."
- **Summary** as label / value rows, labels in `secondary` in a 120 px column:
  LayerD, Active profile, Visual worker, Last error. LayerD and Visual worker
  lead with a StatusText mark and word: ✓ Connected, ! Quarantined or
  Degraded, ✕ Not connected or Stopped. Not connected adds what to do: "Start
  LayerC to run LayerD, then refresh." A value that doesn't exist yet reads
  "Unavailable" in `disabled`.
- **Details:** a semibold heading with Summary / Raw JSON radio buttons,
  disabled until a snapshot arrives, over a read-only box in mono `verbose`
  (Services, Sessions, Automation faults, overlay item diagnostics and, on
  Linux, X11, capabilities and display). Without a snapshot the box holds an
  EmptyState: "No snapshot yet." and "LayerD reports one while it runs."
- **Buttons:** Refresh snapshot, Retry visual (only while a helper is
  quarantined) and Copy raw JSON (disabled without a snapshot) at the left;
  Close at the right.
- **Source:** `runtime_diagnostics_view` returns each summary row with its
  `StatusKind`, so the marks never come from parsing the text.

The consumer supplies the connection state and the snapshot. The values shown
are sample data.

Preview:
[`components/DiagnosticsDialog/preview.html`](design-system/components/DiagnosticsDialog/preview.html).

### Dialog

The frame every dialog shares: a caption, content with `win-margin` (12 px)
margins and `win-row-pitch` rows, and one row of buttons along the bottom.

- **Order:** OK · Cancel · Apply at the right, the Windows order and the one
  Options uses; a reset button alone at the left. One primary button: the
  default action.
- **Buttons:** 12 px from the bottom and side edges, 8 px apart, every one
  `win-control-height` (26 px) and at least 88 px wide (text plus 24 px when
  longer).
- **Layout:** code lays every dialog out with `DialogLayout` after the
  resource template loads (`editor_dialog_layouts.cpp`), and refits it when
  the text size or DPI changes; template sizes never reach the screen.
  Hints sit under their control in `subtext`, indented to its label. Options
  measures labels and wrapped hints in their own font rather than relying on
  template widths. Resizable dialogs derive their minimum extent from the
  same layout that places their content, including wrapped validation text.
- **Placement:** content fitting preserves the user's position while the
  visible frame fits its monitor's work area; otherwise it moves only as far
  as needed to keep the frame on that work area. Expanding Appearance must
  keep its bottom actions visible when its content fits the work area.
- **By kind:** settings dialogs (Options, Appearance) use OK · Cancel · Apply,
  with Reset all at the left in Appearance; tool windows (Automation) use a
  primary Close; messages (Keyboard shortcuts) use a primary OK; a
  confirmation puts its action first as primary, then the alternative, then
  Cancel; an editing dialog names its primary for what it does (Add item, Save
  item) before Cancel.
- Enter presses the primary button and Esc presses Cancel or Close.

The consumer supplies the caption, the content and which kind of dialog it is.
The preview is the real Options dialog laid out by these rules.

The preview is a static HTML rendition of these rules; the painters named
above are the source of truth. The Screens pages show it in place.

Preview:
[`components/Dialog/preview.html`](design-system/components/Dialog/preview.html).

### DropDown

A drop-down list painted like an Edit: square, `input` fill, 1 px `border`
outline, text inset 8 px, with a chevron centered in a `win-chevron-zone` (26
px) zone at its right.

- **Height:** `win-combo-height` (23 px), the native combo box's own height,
  not `win-control-height`.
- **Hover and open:** `row_hover` fill.
- **Focused or open:** a 2 px `accent` ring (`focus-width`) in place of the 1
  px outline, as on an Edit.
- **Disabled:** the window's fill (`background`), `divider` outline,
  `disabled` text and chevron.
- **The list itself** keeps the dark theme.
- **Linux:** an FLTK choice with the same ring; no tint on focus.

The consumer supplies the options, the current value, a label and the enabled
state. Painted by `paint_flat_combo` (Windows).

The preview is a static HTML rendition of these rules; the painters named
above are the source of truth. The Screens pages show it in place.

Preview:
[`components/DropDown/preview.html`](design-system/components/DropDown/preview.html).

### Edit

A single-line text field: `input` fill in a 1 px `border` outline, text inset
6 px and vertically centered.

- **Focus:** a 2 px `accent` ring (`focus-width`) in place of the 1 px
  outline, on every surface and both platforms.
- **Disabled:** `input` fill kept, so it still reads as a field (the code's
  stated intent, `editor_draw.cpp`); `divider` outline, `disabled` text.
  Read-only edits keep the same fill.
- **Long values** such as an application path show their end, so the file name
  stays visible; the tooltip carries the full value.
- **Size:** `win-control-height` on Windows, at most `win-field-cap` wide;
  `linux-input-height` on Linux.
- **Label:** on Windows the row label sits to the left in `secondary`, shares
  the row height (`DialogLayout::row_label`, `SS_CENTERIMAGE`) and carries the
  field's access key (Profile &name); on Linux it sits above the field
  (`FL_ALIGN_TOP_LEFT`).

The consumer supplies the value, the label and the enabled state. Never round
the corners or change the fill on focus: the ring is the whole focus signal.

The preview is a static HTML rendition of these rules; the painters named
above are the source of truth. The Screens pages show it in place.

Preview:
[`components/Edit/preview.html`](design-system/components/Edit/preview.html).

### EmptyState

What an empty list shows: centered inside the list, one `secondary` line
saying what is missing and one `subtext` line saying how to add it.

- Use the words of the control that adds the thing: "Choose a preset above and
  press New."
- Never leave a list blank, and never put the explanation beside the list
  instead of in it.

The consumer supplies the two lines.

The preview is a static HTML rendition of these rules.

Preview:
[`components/EmptyState/preview.html`](design-system/components/EmptyState/preview.html).

### ListRow

A row in a list on `input`: `row_hover` under the pointer, `selection` with a
3 px `accent` rail (`accent-rail`) on its left edge when selected.

- **Keyboard focus:** the focused list gets a 2 px `accent` bottom edge, and
  its focused row a 2 px `accent` ring in place of the rail.
- **Profile rows** (`win-profile-row`, 21 px): a square CheckBox, the profile
  name in `text`, its executable in `secondary`, and an "Off" tag in
  `secondary` at the right when the profile is switched off. "Off" is a state,
  so it never uses `disabled`.
- **Item rows** (`win-list-row`, 27 px): cooldowns ("Dash · 6.5 s · Shift +
  C"), panel items ("Cooldown: Dash"), control roles; text inset 10 px from
  the rail.
- The list is inset 1 px inside its `border` outline.
- `text` reads at 9.7:1 or more on `selection` in every preset.

The consumer supplies the rows, the selected row, the focused row and the
hovered row.

The preview is a static HTML rendition of these rules; the painters named
above are the source of truth. The Screens pages show it in place.

Preview:
[`components/ListRow/preview.html`](design-system/components/ListRow/preview.html).

### MenuBar

A `background` strip with a 1 px `divider` line along its bottom: File, Edit,
View, Tools, Help. Items take `button_hover` under the pointer.

- **The menus:** File, Edit, View, Tools, Help, each command in one of them;
  Menus lists their contents and the context menus. Sentence case throughout.
- **Shortcuts** sit right-aligned in each item (`\t` in the menu resource).
- **Drop-down menus:** the dark system menu on Windows
  (`install_dark_menu_bar`); outlined flat boxes on Linux, whose bar line is
  halfway between its outline and fill.
- **Text:** `text`, in the body style.

The consumer supplies the menu titles and their commands.

The preview is a static HTML rendition of these rules; the painters named
above are the source of truth. The Screens pages show it in place.

Preview:
[`components/MenuBar/preview.html`](design-system/components/MenuBar/preview.html).

### Menus

Every command has one home in the menu bar; a context menu is that command's
group for the row you clicked.

- **File:** every profile command, since the profile is the document: New
  profile (Ctrl+N), Duplicate profile, Rename profile, Turn on / Turn off
  profile, Delete profile... · Save (Ctrl+S), Save as... (Ctrl+Shift+S) ·
  Import pack..., Export pack... · Open profiles folder · Exit.
- **Edit:** Undo (Ctrl+Z), Redo (Ctrl+Y) · Add cooldown, Duplicate cooldown
  (Ctrl+D), Rename cooldown, Remove cooldown... · Add item..., Edit item...,
  Move up, Move down, Remove item... · Split grouped bars....
- **View:** Show advanced settings (Ctrl+Shift+A) · Text size: 100 %, 115 %,
  130 % as radio items. Text size has no other home; Options doesn't repeat
  it.
- **Tools:** Options... (Ctrl+,), Appearance..., Automation... · Runtime
  diagnostics....
- **Help:** Keyboard shortcuts (F1) · About LayerC Editor.
- **Context menus** for a profile row, a cooldown row, a panel item row and an
  empty profile list (New profile, Import pack...). They use the menu bar's
  verbs, order and shortcuts, without the noun, because the row is the object:
  Rename, Duplicate, Remove....
- **Shortcuts shown:** keys that always do the same thing appear in the menu
  bar; F2 and Del act on the focused list, so they appear in context menus and
  in Keyboard shortcuts.
- **Access keys** are unique within each menu (Duplicate cooldown takes C,
  since U is Undo's).
- **Words:** Delete takes a file off disk (profiles); Remove takes something
  out of a profile (cooldowns, panel items). "..." marks an item that opens a
  window or a confirmation.

The consumer supplies the commands and their enabled states; Linux builds the
same menus from its command registry.

Preview:
[`components/Menus/preview.html`](design-system/components/Menus/preview.html).

### NumberField

An Edit that holds a number: same fill, outline, focus ring and size, with no
spin arrows.

- **Stepping:** Up/Down or the mouse wheel. Duration steps 0.1 s (Shift: 1 s);
  bar width and height step 1 px. The control's tooltip says so ("Up/Down or
  the mouse wheel step 0.1 s (Shift: 1 s)").
- The unit goes in the label, not the field: "Duration (s)", "Active (ms)".
- Explain a special value with a `subtext` hint after the field: "0 = off".
- On an invalid value, keep the person's text and say why in a `no` line under
  the field; the status line says "Not autosaved — finish the invalid field."

The consumer supplies the value, its step, range and unit.

The preview is a static HTML rendition of these rules; the painters named
above are the source of truth. The Screens pages show it in place.

Preview:
[`components/NumberField/preview.html`](design-system/components/NumberField/preview.html).

### OverlayBar

The in-game bar in a profile's own colors, checked against the Overlay rules:
text 4.5:1 on fill and background, marks 3:1 on the background.

- **Example profile** (from the repository README): fill #50BAC8, background
  #202A31, border #80D8E8, text #F1F5F7. Its text is 2.08:1 on the fill;
  #141414 is 8.07:1.
- **iwcs:** text #F1F5F7 on fill #DDBC17 is 1.70:1; #141414 is 9.90:1.
- **Contrast line:** under the Fill, Background, Border and Text buttons, while
  a pair is below its minimum, one `warning` line names the pairs and the
  target: "! Low contrast. Text is 1.7:1 on Fill and 1.9:1 on Background;
  aim for 4.5:1." A Border below 3:1 on Background adds "Border is 1.4:1 on
  Background; aim for 3:1." The line goes once every pair passes; it never
  changes the profile or blocks a save.
- **Choose color** opened from those buttons shows the lowest ratio the color
  makes against its partners: Text against Fill and Background, Fill and
  Background against Text, Border against Background. "! Contrast with Fill:
  1.7:1; aim for 4.5:1."

The consumer is the overlay renderer; the profile supplies every color and
size. Item shapes in the preview are illustrative.

The preview is a static HTML rendition of these rules.

Preview:
[`components/OverlayBar/preview.html`](design-system/components/OverlayBar/preview.html).

### OverlayPreview

How the editor shows the overlay: in Add overlay item and Edit overlay item,
at actual size, and in the main window's preview corner. It draws like
`overlay_render.cpp`, not in the editor's tokens, so what you see is what the
game shows, vivid colors included.

- **One painter.** The preview uses the overlay's own geometry
  (`panel_geometry`, `bar_geometry`, `item_pip_layout`) and metrics: Segoe UI
  Semibold 14 pt labels on one line, centered and cut with an ellipsis; bar
  corners `corner_radius` 8 and fills `fill_radius` 6 (RoundRect ellipse
  sizes); cards 78 × 72; pips 8 px apart by 3 px; a grouped bar as a track
  with a 2 px inset fill; the selected panel outlined 2 px #E0A15A, the
  renderer's edit-mode pen. Never a second drawing of the overlay.
- **Item dialog.** Fields in one column at the left: Kind, Source, Label
  (Custom check box and its edit), the kind's own fields (Timer source,
  Counter max and Display, Flash (Hz), Interval), then Color. The preview sits
  at the right in a 300 × 150 well at 1:1, with the panel named under it
  ("Panel “Buffs” · card · row").
- **Show** picks the state: Ready or Cooling (cooldown); Running or Stopped
  (timer); a value (counter); Lit, Lit flashing or Off (indicator);
  Unavailable for every kind. **Backdrop** picks Dark or Bright, to check the
  overlay over a light game.
- **Motion.** A cooldown plays its duration and an indicator flashes at
  min(flashHz, 3) Hz. Both stop when Windows' Show animations is off
  (`SPI_GETCLIENTAREAANIMATION`); Linux starts still. Show still picks any
  state by hand.
- **Readability lines** under the preview, one StatusText line each, warning
  but never blocking ([Overlay](#overlay)): text against fill and against
  background at 4.5:1, marks against background at 3:1, and a label cut to fit
  ("“Shield up” is cut to fit the 78 px card.").
- **Color:** a color button showing the item's hex, starting as the panel's
  fill (`OverlayItem::color`); Choose color opens with the overlay floors.
- **Buttons:** Add item when adding, Save item when editing, as the primary;
  then Cancel.
- **Preview corner:** the same painter scaled into the target frame, with the
  selected item at actual size under it.
- **Screen readers:** the preview is one image whose description names the
  item, its state and any readability line.

The consumer supplies the profile, the panel, the item and the state to show.
The overlay colors in the preview are sample profiles.

Preview:
[`components/OverlayPreview/preview.html`](design-system/components/OverlayPreview/preview.html).

### Scrollbar

A thin, square scrollbar inside lists and the preview: a 12 px track in
`input` with a 1 px `divider` edge, and a 6 px `border` thumb (3.5:1 on
`input`) that turns `secondary` under the pointer.

- No arrow buttons and no rounded thumb.
- Lists scroll rather than overlap when the window is small, as the Linux main
  window already does.
- Keyboard: arrow keys move the selection and the list follows it; Page Up and
  Page Down move a page.

The consumer supplies the scroll range and position.

The preview is a static HTML rendition of these rules.

Preview:
[`components/Scrollbar/preview.html`](design-system/components/Scrollbar/preview.html).

### ShortcutsDialog

Help > Keyboard shortcuts (F1): a message Dialog with a primary OK.

- **Groups** in menu order, each under a semibold heading: File, Edit, In a
  list, In a field or the preview, View, tools and help.
- **One shortcut per row:** the keys in one column as wide as its longest
  entry, the action in `text` beside them. "Ctrl+S / Ctrl+Shift+S" becomes two
  rows.
- **Key chips:** each key in a square chip, `panel` fill inside a 1 px
  `border`, 20 px tall with 6 px side padding, body text; "+" and "/" between
  chips in `secondary`.
- A `subtext` line under the list for input that isn't a key: "The mouse wheel
  steps a focused number like Up and Down."
- **Source:** the command table (`editor_shortcuts.cpp`; Linux
  `shortcut_help`). A command's group is its menu, so a new shortcut lands in
  the right group without an edit.
- **Screen readers:** each row reads as one item, "Ctrl+Shift+S, Save as";
  chips are drawn, not separate controls.

The consumer supplies the command table.

Preview:
[`components/ShortcutsDialog/preview.html`](design-system/components/ShortcutsDialog/preview.html).

### StatusStrip

The Windows main window's bottom strip: `background` fill, a 1 px `divider`
line along its top, `secondary` text, `win-status-strip` (28 px) tall.

- **Left:** the outcome of the last action ("Ready — connecting to LayerD.").
- **Right:** visual recognition state, then LayerD state ("Visual recognition:
  connecting", "LayerD: not running").

- **With the status colors:** each state leads with its glyph and status word:
  ✕ LayerD: not running in `no`, ✓ LayerD: connected in `yes`, … Visual
  recognition: connecting in `accent_2`. The action outcome on the left stays
  `secondary`. StatusText shows it.

The consumer supplies the three messages and each state's `StatusKind` (see
StatusText). Linux shows the same information in fixed regions instead (Save /
reload outcome; LayerD status, in the body size). Explicit outcomes and
failures are announced ([Screen readers](#screen-readers)).

The preview is a static HTML rendition of these rules; the painters named
above are the source of truth. The Screens pages show it in place.

Preview:
[`components/StatusStrip/preview.html`](design-system/components/StatusStrip/preview.html).

### StatusText

A status or information message: a glyph and a semibold status word in `yes`,
`no`, `warning` or `accent_2`, then the message.

- **yes ✓** worked or healthy; **no ✕** failed or stopped; **warning !** works
  but needs a look; **accent_2 …** neither good nor bad (in progress, notes,
  links).
- **Glyphs** are the drawn ones from the Glyphs group, 12 px, in the status
  color; never font characters.
- The glyph or word is required: the hue alone is not enough under red–green
  color blindness, and `no` matches Copper's accent in lightness.
- **In a validation or log box:** glyph in the status color, the message in
  `text`, detail lines in `verbose` (mono).
- **On a selected row:** only the glyph keeps its color; the message is
  `text`.
- **As a fill** (a pip or badge): the status color with `on_status` text.
  Never a control fill, focus or selection.
- **Hints** next to fields use `subtext`, not a status color.

The consumer supplies a `StatusKind` (`src/editor/common/status_kind.hpp`:
`success`, `failure`, `warning`, `information` or `none`) and the message. The
kind is assigned where the outcome is known and carried beside the message;
renderers never read it back from the text. LayerD sends its visual
recognition state as text only, so the Windows editor reads that one known
vocabulary in `visual_runtime_status_kind`, and anything it does not know is
`information`. Windows maps the kind with `status_glyph` and draws it with
`paint_status_glyph` through `set_status_line` and `set_strip_status`; Linux
uses `EditorStatusMark` and `draw_editor_status_glyph`. The adjacent text
carries the accessible message; the decorative glyph is not a second tab stop
or announcement.

The preview is a static HTML rendition of these rules.

Preview:
[`components/StatusText/preview.html`](design-system/components/StatusText/preview.html).

### Tabs

Linux tabs: flat squares in a row. The selected tab carries the `selection`
tint and a 3 px `accent` bar along its top (`accent-rail`); unselected tabs
use `panel`.

- The bar is the signal: the tint alone is at most 1.3:1 against an
  unselected tab, the bar 4.7:1 or more against `panel` in every preset.
- A focused tab takes the 2 px `accent` ring.
- Outlines follow `control_border` (`linux-outline-panel` on a `panel` tab).
- The main window's tabs run from Profile to Visual (270, 15, 775 × 435).
- Text in `linux-body`.

Windows has no tabbed surface in this system. The consumer supplies the tab
labels and the selected tab.

The preview is a static HTML rendition of these rules; the painters named
above are the source of truth.

Preview:
[`components/Tabs/preview.html`](design-system/components/Tabs/preview.html).

### Tooltip

Windows' own tooltip control in its dark theme (`TOOLTIPS_CLASS` with
`DarkMode_Explorer`, at most 360 px wide): Windows draws it, so the preview
only approximates it.

- **Where it is used today:** New profile (Ctrl+N), Delete profile, More
  profile actions, Show advanced (what it reveals, Ctrl+Shift+A), the
  application path (the full path), Browse... ("Choose the game's .exe file"),
  Choose running..., Duration ("Up/Down or the mouse wheel step 0.1 s (Shift:
  1 s)"), Capture key, HID binding, Position on window, the preview ("Drag the
  selected bar. Arrow keys nudge it 1 px (Shift: 10 px).") and bar width and
  height.
- **Disabled buttons** carry their reason as the tooltip ("A profile needs at
  least one cooldown."), with the same sentence shown under the button
  ([Button](#button)).
- Write it as one sentence or a fragment; shortcuts in parentheses. Never put
  the only copy of an instruction in a tooltip.
- Give the same text to the control as its accessible description.

The consumer supplies the text and the control it belongs to.

The preview is a static HTML rendition of these rules; the painters named
above are the source of truth. The Screens pages show it in place.

Preview:
[`components/Tooltip/preview.html`](design-system/components/Tooltip/preview.html).

### WindowFrame

The Windows 11 window frame: a dark caption, with caption and border colors
taken from the menu role, and square corners (`DWMWCP_DONOTROUND`).

The frame is drawn by the system (DWM), so it has no preview. Linux windows
take the X11 window manager's frame.

## Overlay

The in-game overlay draws each profile's panels in that profile's own colors,
sizes and corner radius, not in the editor's tokens. These rules keep any
profile readable; the editor checks them while colors are picked and warns,
without blocking. They cover the Win32 and X11 overlays, Wayland/GNOME frames
and the editor's overlay preview, which must stay faithful to the profile.

### Readability

- **Text** reads at 4.5:1 or more against the bar's `fillColor` and against
  its `backgroundColor`. When a light text color fails, suggest #141414. The
  repository [README](../README.md)'s example profile shows the problem:
  #F1F5F7 on #50BAC8 is 2.08:1, and #141414 on it is 8.07:1.
- **Marks** (counter pips, indicators, the bar's edge) reach 3:1 against
  `backgroundColor`.
- **Size:** text at least 12 px at the 1920 × 1080 reference the preview uses
  without a live target.
- **Flashing:** an indicator flashes at 3 Hz at most, the limit WCAG 2.3.1
  sets at three flashes per second. Profiles keep loading and saving
  `flashHz` 0 or 0.5–4 unchanged, so no profile breaks; every renderer
  flashes at min(`flashHz`, 3) through one core helper. The editor's field
  accepts 0 or 0.5–3; a loaded value above 3 stays in the profile and shows
  a drawn `warning` glyph beside "Flashes at 3 Hz; faster flashing is capped."
- **Lit cards:** text on a lit indicator card is #141414 when the item color's
  luma (299R + 587G + 114B) is at least 140000, else the card text #E4E8EC:
  the `on_accent` rule, so a light item color stays readable (#141414 on
  #60B0FF is 8.0:1).
- **Missing data** shows an unavailable row, never a made-up zero.

### What a profile sets

| Field | Meaning |
| --- | --- |
| `width`, `height` | Bar size in device-independent pixels, scaled with the target's client area |
| `fillColor` | The filled part of a bar |
| `backgroundColor` | The unfilled part, behind text |
| `borderColor` | The bar's edge |
| `textColor` | Labels and values on the bar |
| `hideWhenReady` | Hide a cooldown bar when it is ready |
| `anchorX`, `anchorY` | Panel position as a fraction of the target window |
| `corner_radius` | The panel's corners; the only rounded drawing the editor makes |

### Item kinds

| Kind | Source | Shows |
| --- | --- | --- |
| `cooldown` | `cooldownId` | Remaining time and charges |
| `timer` | an automation `timerId`, or `cooldownActive` with a `cooldownId` | Time left, or a stopped row |
| `counter` | a numeric `variable`, optional `max` | A number, or pips when `max` is a whole number from 1 to 8 |
| `indicator` | a boolean or number `variable` | Lit while true or nonzero; may flash |

A typed panel holds 1 to 32 items; a profile holds at most 64 panels. In the
editor's main window every kind fills the same three settings slots (source,
detection, look) under the preview corner; the [Layout
page](design-system/components/MainWindowLayout/preview.html) has the table.
The [OverlayBar preview](design-system/components/OverlayBar/preview.html)
shows these rules on the example profile.

### Editor preview

The editor draws the overlay with the renderer's own painter and metrics, at
actual size in Add overlay item and scaled in the main window's preview corner
([OverlayPreview](#overlaypreview)). Two backdrops stand in for a game: Dark,
a vertical blend #152029 → #1F2D27 → #3A3324, and Bright, #A9CBE5 → #DCE8D4 →
#CDB88B. They are preview constants, not editor tokens. The selected panel
takes the renderer's edit-mode outline, 2 px #E0A15A.

## Known deviations in the shipped editor

Until v2.1 lands, the shipped editor differs from these rules here; don't copy
any of it:

- Text size is in both View and Options.
- In the Edit menu, Undo and Duplicate cooldown share the access key U.
- Rename profile and Turn on are only in the profile's context menu; panel
  items have no menu; Runtime diagnostics sits under Help and Split grouped
  bars under Tools.
- Add overlay item's primary button says "Apply item" though it closes the
  dialog, and the dialog has no preview.
- Runtime diagnostics shows its summary as plain text, and a disabled, checked
  "Show raw JSON" before any snapshot.
- A lit indicator card keeps #E4E8EC text: 1.9:1 on #60B0FF.

The v2 changes are all in the code. Platform and physical evidence limits are
listed under [Verification](#verification).

## Implementation map

| Concern | Windows | Linux |
| --- | --- | --- |
| Tokens and roles | `native_theme.hpp/.cpp` (`neutral_theme`, `native_theme_colors`); shared color math below | `editor_appearance.hpp/.cpp` (`editor_tokens`, `theme_role_colors`, `control_border`); shared color math below |
| Fonts | `create_native_font`, `ui_face` | `editor_style.cpp` (`apply_desktop_font`, `parse_desktop_font`, `xsettings_string`) |
| Control painting | `native_controls.cpp` (`paint_native_button`, `paint_native_check_box`, `paint_flat_combo`, `paint_flat_frame`, menu bar); `native_theme.cpp` (`paint_focus_ring`, `paint_status_glyph`, `high_contrast_colors`) | `editor_style.hpp/.cpp` (`install_editor_style`, `EditorField`, `EditorChoice`, `draw_editor_focus`, `primary_button`); `editor_geometry.cpp` (`EditorTabs::draw`); `editor_menus.cpp` (`ContextBrowser::draw`) |
| Applying a theme | `editor_theme.cpp`; `EditorWindow` drawing in `editor_draw.cpp` | `appearance_window.cpp` (`apply_editor_appearance`) |
| Metrics | `dialog_layout.hpp` (`native_control_height`, `DialogLayout`), `editor_dialog_layouts.cpp` (Options, Appearance, Choose color, Keyboard shortcuts), `editor_layout.cpp`, `editor_automation_layout.cpp` | `editor_input_height`, `editor_button_height` in `editor_style.hpp`; form builders in `editor_pages.cpp`; `desktop_text_metrics` and `apply_desktop_font` for app-local text scaling |
| Contrast checks | `contrast/appearance_contrast.cpp` (Appearance refusal and live lines, Choose color line); floors and wording from the shared `appearance_floor.cpp` | `editor_appearance.cpp` (`first_role_contrast_failure`: Text / Fill, Text / Selection where the selection sits behind text, and each painted pair) and `appearance_window.cpp` (`painted_text_pairs`, warning line, Apply refusal), with the same `appearance_floor.cpp`; on Windows also `shared_appearance_failure` through the Win32 `contrast/appearance_contrast.cpp` |
| Status lines and text tones | `StatusKind` in the shared `status_kind.hpp`; `static_style.hpp` (`status_glyph`, `set_status_line`, `set_strip_status`, `set_text_tone`); `visual_runtime_status.hpp` (`visual_runtime_status_kind`); `EditorWindow::draw_status_line` | `StatusKind` in the shared `status_kind.hpp`; `EditorStatusMark`, `draw_editor_status_glyph`, `StatusPane`; `DaemonStatus::kind` and `save_report_kind` in `daemon_link.hpp` |
| Keyboard shortcuts | `editor_shortcuts.cpp` (command table, `shortcut_rows` from the accelerators) | `editor_commands.cpp` (`shortcut_help`, including F1); `diagnostics_window.cpp` (`show_shortcuts_dialog`, measured browser columns) |
| Startup | dialog chrome on create | `main.cpp` calls `install_editor_style()` before any widget |

Both platforms use `src/editor/common/editor_color.hpp` for color calculations
(`rgb_contrast_ratio`, `rgb_hue_degrees`, `rgb_accent_2`, `rgb_on_accent`); toolkit
color conversion and resources stay in their platform adapters.

## Not yet in the code

The v2 changes are all in the code. The v2.1 changes are not yet:

- **Overlay preview ([OverlayPreview](#overlaypreview)):**
  `editor_item_dialog.cpp` lays the dialog out in two columns, adds Show,
  Backdrop and Color, renames the primary to Add item / Save item, and draws
  the preview with the painter `paint_preview` uses, fed by `panel_geometry`,
  `bar_geometry` and `item_pip_layout`. Readability lines come from
  `appearance_contrast.cpp`'s overlay checks. Linux: the overlay item window
  in `editor_pages.cpp`.
- **Lit cards ([Overlay](#overlay)):** `overlay_render.cpp`, the X11 overlay
  and the editor preview pick #141414 for a lit card's text when the item
  color's luma is at least 140000, through one core helper.
- **Keyboard shortcuts ([ShortcutsDialog](#shortcutsdialog)):**
  `editor_shortcuts.cpp` adds each command's group and draws the key chips;
  Linux `show_shortcuts_dialog`.
- **Runtime diagnostics ([DiagnosticsDialog](#diagnosticsdialog)):**
  `runtime_diagnostics_view` returns rows with a `StatusKind`;
  `IDD_RUNTIME_DIAGNOSTICS` swaps the check box for Summary / Raw JSON radio
  buttons and gains the empty state; Linux `diagnostics_window.cpp`.
- **Choose color and Appearance ([ColorPicker](#colorpicker),
  [AppearanceRoles](#appearanceroles)):** `editor_color_picker.cpp` (target
  line, current and new, a line per pair) and `editor_appearance.cpp` (part
  rows, Changed, a clickable preview), with `appearance_contrast.cpp` for the
  ratios.
- **Menus ([Menus](#menus)):** `IDR_EDITOR_MENU` and `IDR_CONTEXT_MENUS` in
  `editor_resources.rc` (new panel item and empty profile list menus),
  `IDD_OPTIONS` without Text size, and the Linux command registry in
  `editor_commands.cpp`.

The implementation map identifies the production helpers; the tests and
rendered checks below cover each change.

## Rules for changes

- Change a token or metric at its single source; never hard-code a color,
  radius or control height in a dialog.
- New controls reuse the painters; new surfaces use `native_control_height`
  and `DialogLayout` (Windows) or the existing form builders (Linux).
- No `RoundRect`, rounded regions or rounded FLTK box types in editor chrome.
  The overlay preview is the only rounded editor drawing, and it follows the
  profile.
- Accessibility names, tab order and keyboard paths are part of the design:
  restyling must not rename, reorder or hide controls.
- A visual change updates `DesignSystem.md` and its version in the same
  commit, and is checked against these rules in rendered screenshots of both
  platforms.
- Keep the System theme working on both platforms; it is the escape hatch.
- Give every status message its `StatusKind` where the outcome is known and
  pass it with the text. Never pick a glyph or status color by searching the
  message text: a message can mention "saved" or "active" while reporting a
  failure.
- Check contrast by test, not by eye: the floors in [Where each color may
  go](#where-each-color-may-go) belong in `layerc_editor_tests`, the Linux
  outlines in `layerc_linux_editor_model_tests`.

## Verification

1. Build: `cmake --build build/vs2026 --config Release --target layere
   layerc_editor_tests layerc_calibration_tests`; Linux through
   `tools/linux-gui/build.sh` (see [`Linux.md`](Linux.md)).
2. Tests: `layerc_editor_tests`, `layerc_calibration_tests`,
   `layerc_linux_editor_model_tests` (tokens, outlines, font parsing) and
   `layerc_linux_editor_widget_tests` under an X display (layout, System theme
   restore, small-window scrolling, offscreen focus/tab/glyph painting).
3. Rendered check with cua-driver window screenshots: main window empty and
   populated, Automation, Appearance, Options, Detected Tasks; on Linux the
   main window and Appearance under WSLg or a real X11 desktop. Compare
   against the rules above and the previews in
   [`design-system/`](design-system/index.html), not against memory.
   - Windows menus may not open through synthetic clicks; posting the menu
     command (`WM_COMMAND`, ids in `src/editor/editor_resource.h`) to the
     editor window opens dialogs reliably for review.
   - If WSLg host input is unavailable or a verified no-op, drive keyboard
     input from inside the distribution (`xdotool windowactivate <id>;
     xdotool key <keys>`). `windowfocus` alone is not sufficient. Inspect
     each modal transition before sending the next action.
4. Run `python tools/check_design_system.py` and
   `python -m unittest discover -s tools/tests -p test_design_system.py`.
   The first command checks token agreement, color and square-chrome rules,
   control metrics and Writing rules; the tests plant violations to check
   that those classes fail. CI runs both in `ci.yml`'s `workflow-lint` job.
5. The XSETTINGS font path needs a desktop with a settings manager (GNOME,
   KDE); WSLg and Xvfb exercise only the fontconfig fallback. Real Narrator or
   Orca speech, actual contrast-theme settings and GitHub CI need their own
   observed evidence; seam tests and local captures do not establish it.

## Revert or reimplement

- **Inspect:** `git show design-v1-square-native` is the complete v1
  implementation; once v2 lands, `git diff design-v1-square-native
  design-v2-accessible -- src/` is its source delta.
- **Revert the look:** `git revert` the v2 commit restores v1; resolve
  conflicts in the files of the implementation map, then rerun the
  verification above.
- **Reimplement elsewhere** (a new toolkit, branch or rewrite): apply the
  tokens, typography and geometry tables and the component rules; port the
  painters listed in the implementation map; keep the tests named above
  passing, including the Linux helper tests for outlines and font parsing.
- **Evolve:** bump the version here, describe what changed and why under a
  "Changes from" section, tag the commit `design-vN-<name>`, keep the previous
  tag for comparison, and re-export [`design-system/`](design-system/) from
  the design system.
