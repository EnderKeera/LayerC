#pragma once

#include <algorithm>

#include <windows.h>

namespace layerc::editor {

// Shared DPI-scaled dialog plumbing: every resizable editor dialog computes
// the same 96-DPI baseline, logical scaling, and clamped MoveWindow.
[[nodiscard]] inline int window_base_dpi(const HWND window) {
  const UINT dpi = GetDpiForWindow(window);
  return dpi == 0 ? 96 : static_cast<int>(dpi);
}

[[nodiscard]] inline int dpi_scaled(const int logical, const int dpi) {
  return MulDiv(logical, dpi, 96);
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

} // namespace layerc::editor
