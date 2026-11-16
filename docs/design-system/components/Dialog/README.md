# Dialog

The frame every dialog shares: a caption, content with `win-margin` (12 px) margins and `win-row-pitch` rows, and one row of buttons along the bottom.

- **Order:** OK · Cancel · Apply at the right, the Windows order and the one Options uses; a reset button alone at the left. One primary button: the default action.
- **Buttons:** 12 px from the bottom and side edges, 8 px apart, every one `win-control-height` (26 px) and at least 88 px wide (text plus 24 px when longer).
- **Layout:** code lays every dialog out with `DialogLayout` after the resource template loads (`editor_dialog_layouts.cpp`), and refits it when the text size or DPI changes; template sizes never reach the screen. Hints sit under their control in `subtext`, indented to its label. Options measures labels and wrapped hints in their own font rather than relying on template widths. Resizable dialogs derive their minimum extent from the same layout that places their content, including wrapped validation text.
- **Placement:** content fitting preserves the user's position while the visible frame fits its monitor's work area; otherwise it moves only as far as needed to keep the frame on that work area. Expanding Appearance must keep its bottom actions visible when its content fits the work area.
- **By kind:** settings dialogs (Options, Appearance) use OK · Cancel · Apply, with Reset all at the left in Appearance; tool windows (Automation) use a primary Close; messages (Keyboard shortcuts) use a primary OK; a confirmation puts its action first as primary, then the alternative, then Cancel; an editing dialog names its primary for what it does (Add item, Save item) before Cancel.
- Enter presses the primary button and Esc presses Cancel or Close.

The consumer supplies the caption, the content and which kind of dialog it is. The preview is the real Options dialog laid out by these rules.

The preview is a static HTML rendition of these rules; the painters named above are the source of truth. The Screens pages show it in place.
