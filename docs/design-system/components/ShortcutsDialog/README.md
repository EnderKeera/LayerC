# ShortcutsDialog

Help > Keyboard shortcuts (F1): a message Dialog with a primary OK.

- **Groups** in menu order, each under a semibold heading: File, Edit, In a list, In a field or the preview, View, tools and help.
- **One shortcut per row:** the keys in one column as wide as its longest entry, the action in `text` beside them. "Ctrl+S / Ctrl+Shift+S" becomes two rows.
- **Key chips:** each key in a square chip, `panel` fill inside a 1 px `border`, 20 px tall with 6 px side padding, body text; "+" and "/" between chips in `secondary`.
- A `subtext` line under the list for input that isn't a key: "The mouse wheel steps a focused number like Up and Down."
- **Source:** the command table (`editor_shortcuts.cpp`; Linux `shortcut_help`). A command's group is its menu, so a new shortcut lands in the right group without an edit.
- **Screen readers:** each row reads as one item, "Ctrl+Shift+S, Save as"; chips are drawn, not separate controls.

The consumer supplies the command table.
