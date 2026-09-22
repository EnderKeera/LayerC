#pragma once

#include <algorithm>

#include <windows.h>

namespace layerc::editor {

[[nodiscard]] inline HFONT create_editor_font(const UINT dpi) {
  const auto effective_dpi = dpi == 0 ? 96 : static_cast<int>(dpi);
  return CreateFontW(-MulDiv(9, effective_dpi, 72), 0, 0, 0, FW_NORMAL,
                     FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                     L"Segoe UI");
}

inline void apply_suggested_dpi_rect(const HWND window, const LPARAM lparam) {
  const auto *suggested = reinterpret_cast<const RECT *>(lparam);
  if (suggested == nullptr)
    return;
  SetWindowPos(window, nullptr, suggested->left, suggested->top,
               suggested->right - suggested->left,
               suggested->bottom - suggested->top,
               SWP_NOZORDER | SWP_NOACTIVATE);
}

void style_native_dialog_children(HWND dialog, HFONT font);

[[nodiscard]] inline bool replace_dialog_font(const HWND dialog, const UINT dpi,
                                              HFONT &font) {
  const HFONT replacement = create_editor_font(dpi);
  if (replacement == nullptr)
    return false;
  const HFONT previous = font;
  font = replacement;
  style_native_dialog_children(dialog, font);
  if (previous != nullptr)
    DeleteObject(previous);
  return true;
}

inline void destroy_dialog_font(HFONT &font) noexcept {
  if (font != nullptr) {
    DeleteObject(font);
    font = nullptr;
  }
}

// Shared DPI-scaled dialog plumbing: every resizable editor dialog computes
// the same 96-DPI baseline, logical scaling, and clamped MoveWindow.
[[nodiscard]] inline int window_base_dpi(const HWND window) {
  const UINT dpi = GetDpiForWindow(window);
  return dpi == 0 ? 96 : static_cast<int>(dpi);
}

[[nodiscard]] inline int dpi_scaled(const int logical, const int dpi) {
  return MulDiv(logical, dpi, 96);
}

[[nodiscard]] inline int
geometry_dpi_for_extent(const int dpi, const int width, const int height,
                        const int design_width, const int design_height) {
  return std::max(80, std::min({dpi, MulDiv(width, 96, design_width),
                                MulDiv(height, 96, design_height)}));
}

[[nodiscard]] inline int geometry_dpi(const HWND window,
                                      const int design_width,
                                      const int design_height) {
  RECT client{};
  GetClientRect(window, &client);
  return geometry_dpi_for_extent(
      window_base_dpi(window), std::max(1L, client.right - client.left),
      std::max(1L, client.bottom - client.top), design_width, design_height);
}

inline void move_control(const HWND dialog, const int id, const int x,
                         const int y, const int width, const int height) {
  if (const HWND control = GetDlgItem(dialog, id); control != nullptr)
    MoveWindow(control, x, y, std::max(1, width), std::max(1, height), TRUE);
}

inline void resize_to_logical_size(const HWND dialog, const int logical_width,
                                   const int logical_height) {
  const int dpi = window_base_dpi(dialog);
  SetWindowPos(dialog, nullptr, 0, 0, dpi_scaled(logical_width, dpi),
               dpi_scaled(logical_height, dpi),
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

inline void apply_minimum_track_size(const HWND dialog, const LPARAM lparam,
                                     const int logical_width,
                                     const int logical_height) {
  const int dpi = window_base_dpi(dialog);
  auto *const limits = reinterpret_cast<MINMAXINFO *>(lparam);
  limits->ptMinTrackSize = {dpi_scaled(logical_width, dpi),
                            dpi_scaled(logical_height, dpi)};
}

// Owns the shared font and DPI lifecycle for a native editor dialog.
class DialogChrome final {
public:
  DialogChrome() = default;
  ~DialogChrome() { on_destroy(); }
  DialogChrome(const DialogChrome &) = delete;
  DialogChrome &operator=(const DialogChrome &) = delete;

  [[nodiscard]] bool attach(const HWND dialog, UINT dpi = 0) {
    dialog_ = dialog;
    if (dpi == 0 && dialog_ != nullptr)
      dpi = GetDpiForWindow(dialog_);
    return dialog_ != nullptr && replace_dialog_font(dialog_, dpi, font_);
  }

  [[nodiscard]] bool on_dpi_changed(const UINT dpi, const LPARAM lparam) {
    if (dialog_ == nullptr)
      return false;
    apply_suggested_dpi_rect(dialog_, lparam);
    return replace_dialog_font(dialog_, dpi, font_);
  }

  void on_minimum_size(const LPARAM lparam, const int logical_width,
                       const int logical_height) const {
    if (dialog_ != nullptr)
      apply_minimum_track_size(dialog_, lparam, logical_width, logical_height);
  }

  void on_destroy() noexcept {
    destroy_dialog_font(font_);
    dialog_ = nullptr;
  }

  [[nodiscard]] HWND window() const noexcept { return dialog_; }
  [[nodiscard]] HFONT font() const noexcept { return font_; }

private:
  HWND dialog_{};
  HFONT font_{};
};

} // namespace layerc::editor
