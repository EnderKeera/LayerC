# ContextMenu

The menu opened by right-click or Shift+F10 on a list row: a flat box with a 1 px `border` outline on `background` (Linux), or the dark system menu (Windows), with each item's shortcut right-aligned in `secondary`.

- Items are 24 px tall, `button_hover` under the pointer, `disabled` when unavailable; a 1 px `divider` separates groups.
- Offer only what applies to the row, with the Edit menu's names and shortcuts: Rename cooldown (F2), Duplicate cooldown (Ctrl+D), Remove cooldown... (Del).
- Every item has a shortcut or access key, and the menu closes with Esc.

The consumer supplies the items, their shortcuts and enabled states. The items shown are an example.

The preview is a static HTML rendition of these rules.
