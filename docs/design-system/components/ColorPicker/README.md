# ColorPicker

Choose color, opened from a color button in Appearance, the main window or Add overlay item. A Dialog with OK · Cancel.

- **Target line** at the top: what is being picked, semibold, then where in `secondary`: "Window fill · Graphite / Copper", "Fill · panel iwcs".
- **Palette:** 24 swatches, 8 × 3, 44 × 26 px, 6 px apart, each with a 1 px `border`. The chosen swatch takes a 2 px `accent` ring 1 px outside it. Arrow keys move between swatches, and each is named for screen readers ("Copper #D88E4B, 7 of 24").
- **Custom color:** Red, Green and Blue as NumberFields (0–255, Up and Down step) and Hex in the mono face, with the hint "0 to 255 each, or #RRGGBB. Up and Down step a value."
- **Current and New:** two 200 × 44 samples side by side showing the role's real text on its fill, before and after.
- **Contrast:** one StatusText line for each pair the color takes part in, with its ratio and floor: "✓ Text 12.4:1 needs 4.5:1". Below an editor floor the line is `warning` and ends "Appearance won't save it."; for an overlay color the floors are the Overlay rules and the line only warns.

The consumer supplies the target, its current color and the pairs to check.
