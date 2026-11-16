# Menus

Every command has one home in the menu bar; a context menu is that command's group for the row you clicked.

- **File:** every profile command, since the profile is the document: New profile (Ctrl+N), Duplicate profile, Rename profile, Turn on / Turn off profile, Delete profile... · Save (Ctrl+S), Save as... (Ctrl+Shift+S) · Import pack..., Export pack... · Open profiles folder · Exit.
- **Edit:** Undo (Ctrl+Z), Redo (Ctrl+Y) · Add cooldown, Duplicate cooldown (Ctrl+D), Rename cooldown, Remove cooldown... · Add item..., Edit item..., Move up, Move down, Remove item... · Split grouped bars....
- **View:** Show advanced settings (Ctrl+Shift+A) · Text size: 100 %, 115 %, 130 % as radio items. Text size has no other home; Options doesn't repeat it.
- **Tools:** Options... (Ctrl+,), Appearance..., Automation... · Runtime diagnostics....
- **Help:** Keyboard shortcuts (F1) · About LayerC Editor.
- **Context menus** for a profile row, a cooldown row, a panel item row and an empty profile list (New profile, Import pack...). They use the menu bar's verbs, order and shortcuts, without the noun, because the row is the object: Rename, Duplicate, Remove....
- **Shortcuts shown:** keys that always do the same thing appear in the menu bar; F2 and Del act on the focused list, so they appear in context menus and in Keyboard shortcuts.
- **Access keys** are unique within each menu (Duplicate cooldown takes C, since U is Undo's).
- **Words:** Delete takes a file off disk (profiles); Remove takes something out of a profile (cooldowns, panel items). "..." marks an item that opens a window or a confirmation.

The consumer supplies the commands and their enabled states; Linux builds the same menus from its command registry.
