#pragma once

#include <string>

#include <windows.h>

namespace layerc::platform {

inline constexpr char editor_fltk_window_class[] = "LayerC.LayerE.FLTK";

[[nodiscard]] std::wstring editor_single_instance_name();

using EditorImageMatch = bool (*)(DWORD, const wchar_t *);
[[nodiscard]] HWND raise_existing_editor(EditorImageMatch matches_image);
void raise_existing_editor();

} // namespace layerc::platform
