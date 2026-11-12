# Edit

A single-line text field: `input` fill in a 1 px `border` outline, text inset 6 px and vertically centered.

- **Focus:** a 2 px `accent` ring (`focus-width`) in place of the 1 px outline, on every surface and both platforms.
- **Disabled:** `input` fill kept, so it still reads as a field (the code's stated intent, `editor_draw.cpp`); `divider` outline, `disabled` text. Read-only edits keep the same fill.
- **Long values** such as an application path show their end, so the file name stays visible; the tooltip carries the full value.
- **Size:** `win-control-height` on Windows, at most `win-field-cap` wide; `linux-input-height` on Linux.
- **Label:** on Windows the row label sits to the left in `secondary`, shares the row height (`DialogLayout::row_label`, `SS_CENTERIMAGE`) and carries the field's access key (Profile &name); on Linux it sits above the field (`FL_ALIGN_TOP_LEFT`).

The consumer supplies the value, the label and the enabled state. Never round the corners or change the fill on focus: the ring is the whole focus signal.

The preview is a static HTML rendition of these rules; the painters named above are the source of truth. The Screens pages show it in place.
