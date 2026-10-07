#pragma once

#include <windows.h>

#include <optional>

namespace layerc::editor {

// Themed painting for a sub-editor dialog, provided by its host window.
class DialogTheme {
public:
  virtual void draw_list_box_item(const DRAWITEMSTRUCT &item) const = 0;
  [[nodiscard]] virtual std::optional<LRESULT>
  dialog_theme_message(HWND window, UINT message, WPARAM wparam,
                       LPARAM lparam) const = 0;
  // Applies the host's fonts and control styles to a dialog's children.
  virtual void style_dialog_children(HWND dialog) const = 0;

protected:
  ~DialogTheme() = default;
};

} // namespace layerc::editor
