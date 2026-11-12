# DropDown

A drop-down list painted like an Edit: square, `input` fill, 1 px `border` outline, text inset 8 px, with a chevron centered in a `win-chevron-zone` (26 px) zone at its right.

- **Height:** `win-combo-height` (23 px), the native combo box's own height, not `win-control-height`.
- **Hover and open:** `row_hover` fill.
- **Focused or open:** a 2 px `accent` ring (`focus-width`) in place of the 1 px outline, as on an Edit.
- **Disabled:** the window's fill (`background`), `divider` outline, `disabled` text and chevron.
- **The list itself** keeps the dark theme.
- **Linux:** an FLTK choice with the same ring; no tint on focus.

The consumer supplies the options, the current value, a label and the enabled state. Painted by `paint_flat_combo` (Windows).

The preview is a static HTML rendition of these rules; the painters named above are the source of truth. The Screens pages show it in place.
