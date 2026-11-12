# StatusText

A status or information message: a glyph and a semibold status word in `yes`, `no`, `warning` or `accent_2`, then the message.

- **yes ✓** worked or healthy; **no ✕** failed or stopped; **warning !** works but needs a look; **accent_2 …** neither good nor bad (in progress, notes, links).
- **Glyphs** are the drawn ones from the Glyphs group, 12 px, in the status color; never font characters.
- The glyph or word is required: the hue alone is not enough under red–green color blindness, and `no` matches Copper's accent in lightness.
- **In a validation or log box:** glyph in the status color, the message in `text`, detail lines in `verbose` (mono).
- **On a selected row:** only the glyph keeps its color; the message is `text`.
- **As a fill** (a pip or badge): the status color with `on_status` text. Never a control fill, focus or selection.
- **Hints** next to fields use `subtext`, not a status color.

The consumer supplies the state and the message. Intentional addition: the source document defines no status colors; these were adopted in the design system and are not yet in the code.

The preview is a static HTML rendition of these rules.
