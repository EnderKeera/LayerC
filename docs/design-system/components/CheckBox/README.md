# CheckBox

A 16 px square box (`checkbox`) with its label 8 px after it; the whole control, label included, is the click area. Labels that cannot fit on one line wrap, and the control grows to show every line; the box and focus-ring metrics stay unchanged.

- **Unchecked:** `input` fill, `border` outline. Under the pointer the fill turns `button_hover` and the outline `text`; pressed, the fill is `pressed`.
- **Checked:** `accent` fill and outline with an `on_accent` tick: a 2 px mitred line through (4, 9), (7, 12) and (12, 5) in the 16 px box. Under the pointer `primary_hover`; pressed, `accent` blended 60/255 toward the check box's own fill.
- **Disabled:** the role's fill (`background`), `divider` outline, `disabled` label; a checked one fills with `divider` and draws its tick in `disabled`.
- **Focus:** a 2 px `accent` ring around the box and its label, on both platforms.
- **Radio:** a 16 px circle with an 8 px `accent` dot; disabled, the dot turns `disabled`. Runtime diagnostics' Summary / Raw JSON pair uses it (DiagnosticsDialog).
- **Linux:** the FLTK down box, square; the check mark takes the full `accent` as its selection color.

The consumer supplies the label with its access key (&Show text), the checked state and the enabled state. Painted by `paint_native_check_box` through `NM_CUSTOMDRAW` (Windows).

The preview is a static HTML rendition of these rules; the painters named above are the source of truth. The Screens pages show it in place.
