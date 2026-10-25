#pragma once

#include <FL/Enumerations.H>

namespace layerc::editor {

// LayerE's flat, square look on every FLTK surface, matching the Windows
// editor: each up/down/thin/engraved box is a solid fill inside a 1 px
// outline (control_border), and the menu bar keeps only a bottom divider.
// Also adopts the desktop's interface font (XSETTINGS Gtk/FontName, else
// fontconfig's sans) for FLTK's sans faces, at a size the fixed form geometry
// fits. Call once after the display opens and before any widget is built.
void install_editor_style();

// The menu bar's box once the style is installed, else FL_UP_BOX.
[[nodiscard]] Fl_Boxtype editor_menu_bar_box() noexcept;

} // namespace layerc::editor
