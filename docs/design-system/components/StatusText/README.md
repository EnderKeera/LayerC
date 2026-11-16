# StatusText

A status or information message: a glyph and a semibold status word in `yes`, `no`, `warning` or `accent_2`, then the message.

- **yes ✓** worked or healthy; **no ✕** failed or stopped; **warning !** works but needs a look; **accent_2 …** neither good nor bad (in progress, notes, links).
- **Glyphs** are the drawn ones from the Glyphs group, 12 px, in the status color; never font characters.
- The glyph or word is required: the hue alone is not enough under red–green color blindness, and `no` matches Copper's accent in lightness.
- **In a validation or log box:** glyph in the status color, the message in `text`, detail lines in `verbose` (mono).
- **On a selected row:** only the glyph keeps its color; the message is `text`.
- **As a fill** (a pip or badge): the status color with `on_status` text. Never a control fill, focus or selection.
- **Hints** next to fields use `subtext`, not a status color.

The consumer supplies a `StatusKind` (`src/editor/common/status_kind.hpp`: `success`, `failure`, `warning`, `information` or `none`) and the message. The kind is assigned where the outcome is known and carried beside the message; renderers never read it back from the text. LayerD sends its visual recognition state as text only, so the Windows editor reads that one known vocabulary in `visual_runtime_status_kind`, and anything it does not know is `information`. Windows maps the kind with `status_glyph` and draws it with `paint_status_glyph` through `set_status_line` and `set_strip_status`; Linux uses `EditorStatusMark` and `draw_editor_status_glyph`. The adjacent text carries the accessible message; the decorative glyph is not a second tab stop or announcement.

The preview is a static HTML rendition of these rules.
