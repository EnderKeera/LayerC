#pragma once

#include <cstddef>

#include <windows.h>

namespace layerc::editor {

// Code layouts for the fixed dialogs (DesignSystem.md, Dialog): 12 px
// margins, 26 px controls, 8 px gaps and one button row along the bottom.
// Each measures its text in the children's font, so call it after the
// children are styled; it sizes the dialog to fit but does not move it.
void layout_options_dialog(HWND dialog);
// The advanced band (control roles and overrides) takes space only while
// shown; the contrast line keeps its row either way.
void layout_appearance_dialog(HWND dialog, bool advanced);
void layout_color_picker_dialog(HWND dialog);
// `rows` key and action statics from IDC_SHORTCUT_FIRST, two ids per row.
void layout_shortcuts_dialog(HWND dialog, std::size_t rows);

} // namespace layerc::editor
