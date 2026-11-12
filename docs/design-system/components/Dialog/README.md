# Dialog

The frame every dialog shares: a caption, content with `win-margin` (12 px) margins and `win-row-pitch` rows, and one row of buttons along the bottom.

- **Order:** OK · Cancel · Apply at the right, the Windows order and the one Options uses; a reset button alone at the left. One primary button: the default action.
- **Buttons:** 12 px from the bottom and side edges, 8 px apart, every one `win-control-height` (26 px) and at least 88 px wide. Options, Appearance and Choose color still use their resource-template sizes (32 and 36 px); see Known deviations.
- **By kind:** settings dialogs (Options, Appearance) use OK · Cancel · Apply, with Reset all at the left in Appearance; tool windows (Automation) use a primary Close; messages (Keyboard shortcuts) use a primary OK; a confirmation puts its action first as primary, then the alternative, then Cancel.
- Enter presses the primary button and Esc presses Cancel or Close.

The consumer supplies the caption, the content and which kind of dialog it is. The preview is the real Options dialog laid out by these rules.

The preview is a static HTML rendition of these rules; the painters named above are the source of truth. The Screens pages show it in place.
