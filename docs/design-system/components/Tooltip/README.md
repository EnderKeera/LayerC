# Tooltip

Windows' own tooltip control in its dark theme (`TOOLTIPS_CLASS` with `DarkMode_Explorer`, at most 360 px wide): Windows draws it, so the preview only approximates it.

- **Where it is used today:** New profile (Ctrl+N), Delete profile, More profile actions, Show advanced (what it reveals, Ctrl+Shift+A), the application path (the full path), Browse... ("Choose the game's .exe file"), Choose running..., Duration ("Up/Down or the mouse wheel step 0.1 s (Shift: 1 s)"), Capture key, HID binding, Position on window, the preview ("Drag the selected bar. Arrow keys nudge it 1 px (Shift: 10 px).") and bar width and height.
- **Proposed:** say why a button is disabled ("Used by the panel item Cooldown: Dash."), not yet in the code.
- Write it as one sentence or a fragment; shortcuts in parentheses. Never put the only copy of an instruction in a tooltip.
- Give the same text to the control as its accessible description.

The consumer supplies the text and the control it belongs to.

The preview is a static HTML rendition of these rules; the painters named above are the source of truth. The Screens pages show it in place.
