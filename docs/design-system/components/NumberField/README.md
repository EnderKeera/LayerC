# NumberField

An Edit that holds a number: same fill, outline, focus ring and size, with no spin arrows.

- **Stepping:** Up/Down or the mouse wheel. Duration steps 0.1 s (Shift: 1 s); bar width and height step 1 px. The control's tooltip says so ("Up/Down or the mouse wheel step 0.1 s (Shift: 1 s)").
- The unit goes in the label, not the field: "Duration (s)", "Active (ms)".
- Explain a special value with a `subtext` hint after the field: "0 = off".
- On an invalid value, keep the person's text and say why in a `no` line under the field; the status line says "Not autosaved — finish the invalid field."

The consumer supplies the value, its step, range and unit.

The preview is a static HTML rendition of these rules; the painters named above are the source of truth. The Screens pages show it in place.
