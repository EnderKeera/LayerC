# ListRow

A row in a list on `input`: `row_hover` under the pointer, `selection` with a 3 px `accent` rail (`accent-rail`) on its left edge when selected.

- **Keyboard focus:** the focused list gets a 2 px `accent` bottom edge, and its focused row a 2 px `accent` ring in place of the rail.
- **Profile rows** (`win-profile-row`, 21 px): a square CheckBox, the profile name in `text`, its executable in `secondary`, and an "Off" tag in `secondary` at the right when the profile is switched off. "Off" is a state, so it never uses `disabled`.
- **Item rows** (`win-list-row`, 27 px): cooldowns ("Dash · 6.5 s · Shift + C"), panel items ("Cooldown: Dash"), control roles; text inset 10 px from the rail.
- The list is inset 1 px inside its `border` outline.
- `text` reads at 9.7:1 or more on `selection` in every preset.

The consumer supplies the rows, the selected row, the focused row and the hovered row.

The preview is a static HTML rendition of these rules; the painters named above are the source of truth. The Screens pages show it in place.
