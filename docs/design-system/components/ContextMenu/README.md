# ContextMenu

The menu opened by right-click or Shift+F10 on a list row: a flat box with a 1 px `border` outline on `background` (Linux), or the dark system menu (Windows), with each item's shortcut right-aligned in `secondary`.

- Items are 24 px tall, `button_hover` under the pointer, `disabled` when unavailable; a 1 px `divider` separates groups.
- Offer the menu bar's group for the row, in the same order, with the same verbs and shortcuts but without the noun, because the row is the object: Rename (F2), Duplicate (Ctrl+D), Remove... (Del). Menus lists every context menu.
- F2 and Del act on the focused list, so they appear here rather than in the menu bar.
- Every item has a shortcut or access key, and the menu closes with Esc.

The consumer supplies the items, their shortcuts and enabled states. The items shown are an example.

The preview is a static HTML rendition of these rules.
