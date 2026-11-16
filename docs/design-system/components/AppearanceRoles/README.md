# AppearanceRoles

Tools > Appearance with advanced colors shown. A settings Dialog: Reset all at the left; OK · Cancel · Apply at the right.

- **Preset** drop-down at the top left; Show / Hide advanced colors at the right.
- **Preview:** a small editor drawn in the current colors: menu bar, profile list with a selected row, a labelled field, primary, neutral and disabled buttons, a checked box and a status strip with a mark. Clicking a part selects its role.
- **Roles and colors:** the Control role list (168 px) at the left; at the right, "<Role> colors" with one row per color part the role has (Fill, Text, Border, Hover, Selection, Disabled): its label, a color button with swatch and hex (132 px), and its contrast as StatusText ("✓ 14.8:1 on fill"). Parts a role doesn't have aren't shown. A part that differs from the preset adds `accent_2` "Changed". Reset selected sits under the rows.
- **Floors** as in Roles: Text / Fill 4.5:1, Disabled / Fill 3:1, Text / Hover 4.5:1, Text / Selection 4.5:1. Apply and OK refuse below them, naming the role, the pair and the ratio; a contrast theme sets the overrides aside.

The consumer supplies the preset, the roles and their overrides.
