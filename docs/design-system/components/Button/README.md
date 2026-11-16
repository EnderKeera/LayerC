# Button

A square fill inside a 1 px outline, label centered; at most one primary per surface.

- **Neutral:** `panel` fill, `border` outline, `text` label; `button_hover` under the pointer, `pressed` while pressed.
- **Primary:** the dialog's default button (`DM_GETDEFID`), one per surface (OK, Close). `accent` fill and outline, `on_accent` label; `primary_hover` under the pointer, `primary_pressed` while pressed.
- **Disabled:** `panel` fill kept, `divider` outline, `disabled` label.
- **Why disabled:** when the reason isn't visible nearby, a one-sentence `subtext` line under the button says what holds it ("A profile needs at least one cooldown.", "A panel needs at least one item."), and the same sentence is the button's tooltip and accessible description. The line goes when the button is enabled. Never only a tooltip: a disabled control may not show one.
- **Color button:** a `swatch` (16 px, with its own 1 px `border` outline) inset `swatch-inset` (8 px) from the left edge, then 8 px, then the left-aligned text (Fill, Background, Border, Text).
- **Focus:** a 2 px ring inside the edge (`paint_focus_ring`): `accent` on neutral buttons, `text` on the primary one.
- **Access key:** one letter (`&Browse...`), underlined when Windows shows keyboard cues (after Alt, or always with "Underline access keys" on).
- **Size:** `win-control-height` on Windows; `linux-button-height` on Linux, outlined in `linux-outline-panel`.

The consumer supplies the label with its access key, whether it is the surface's default action, its enabled state and, for a color button, the color. Real labels: Show advanced, Browse..., Choose running..., Add cooldown, Remove cooldown, Capture key, Calibrate..., Add..., Edit..., Remove, Move up, Move down, Position on window, Validate, Close.

Painted by `paint_native_button` (Windows) and the flat box types of `install_editor_style` (Linux).

The preview is a static HTML rendition of these rules; the painters named above are the source of truth. The Screens pages show it in place.
