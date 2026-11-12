# StatusStrip

The Windows main window's bottom strip: `background` fill, a 1 px `divider` line along its top, `secondary` text, `win-status-strip` (28 px) tall.

- **Left:** the outcome of the last action ("Ready — connecting to LayerD.").
- **Right:** visual recognition state, then LayerD state ("Visual recognition: connecting", "LayerD: not running").

- **With the status colors:** each state leads with its glyph and status word: ✕ LayerD: not running in `no`, ✓ LayerD: running in `yes`, … Visual recognition: connecting in `accent_2`. The action outcome on the left stays `secondary`. StatusText shows it.

The consumer supplies the three messages. Linux shows the same information in fixed regions instead (Save / reload outcome; LayerD status, in the body size). Explicit outcomes and failures are announced to screen readers.

The preview is a static HTML rendition of these rules; the painters named above are the source of truth. The Screens pages show it in place.
