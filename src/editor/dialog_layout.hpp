#pragma once

#include "layerc/platform/native_theme.hpp"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

#include <windows.h>

namespace layerc::editor {

// The one control height (logical px) of every code-laid-out surface: square
// buttons, edits and drop-down lists sized for the 12 px native body font.
inline constexpr int native_control_height = 26;

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
  const HFONT replacement = layerc::platform::create_native_dialog_font(dpi);
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
// the same 96-DPI baseline, logical scaling, and clamped MoveWindow. The
// baseline includes the text size (Windows setting and Options > Text size)
// so code layouts grow with their text. Fonts take the raw window DPI.
[[nodiscard]] inline int window_base_dpi(const HWND window) {
  const UINT dpi = layerc::platform::native_window_dpi(window);
  return MulDiv(static_cast<int>(dpi),
                layerc::platform::native_text_percent(dpi), 100);
}

[[nodiscard]] inline int dpi_scaled(const int logical, const int dpi) {
  return MulDiv(logical, dpi, 96);
}

inline void move_control(const HWND dialog, const int id, const int x,
                         const int y, const int width, const int height) {
  if (const HWND control = GetDlgItem(dialog, id); control != nullptr)
    MoveWindow(control, x, y, std::max(1, width), std::max(1, height), TRUE);
}

// A control's own text measured in its font, in physical pixels: one line,
// or wrapped to `wrap_width` and as tall as its lines. Access-key markers
// don't count. The height is the font's line height even for empty text.
[[nodiscard]] inline SIZE control_text_extent(const HWND control,
                                              const int wrap_width = 0) {
  SIZE size{};
  if (control == nullptr)
    return size;
  const int length = GetWindowTextLengthW(control);
  std::wstring text(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
  text.resize(static_cast<std::size_t>(
      GetWindowTextW(control, text.data(), length + 1)));
  const HDC dc = GetDC(control);
  if (dc == nullptr)
    return size;
  const auto font =
      reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
  const HGDIOBJ previous = font != nullptr ? SelectObject(dc, font) : nullptr;
  TEXTMETRICW metrics{};
  GetTextMetricsW(dc, &metrics);
  RECT bounds{0, 0, std::max(1, wrap_width), 0};
  if (!text.empty())
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &bounds,
              DT_CALCRECT | DT_NOCLIP |
                  (wrap_width > 0 ? DT_WORDBREAK : DT_SINGLELINE));
  if (previous != nullptr)
    SelectObject(dc, previous);
  ReleaseDC(control, dc);
  size.cx = text.empty() ? 0 : bounds.right - bounds.left;
  size.cy = std::max(static_cast<LONG>(metrics.tmHeight),
                     text.empty() ? 0 : bounds.bottom - bounds.top);
  return size;
}

// Keeps logical units, placement, and repeating rows together for resizable
// native dialogs. Positions remain in physical client pixels.
class DialogLayout final {
public:
  DialogLayout(const HWND dialog, const int dpi) noexcept
      : dialog_(dialog), dpi_(dpi) {}

  [[nodiscard]] int scaled(const int logical) const noexcept {
    return dpi_scaled(logical, dpi_);
  }
  void move(const int id, const int x, const int y, const int width,
            const int height) const {
    move_control(dialog_, id, x, y, width, height);
  }
  // A single-line label sharing a control row: as tall as the row, with its
  // text centered vertically like the text inside the control beside it.
  void row_label(const int id, const int x, const int y, const int width,
                 const int row_height) const {
    if (const HWND control = GetDlgItem(dialog_, id); control != nullptr) {
      const auto style = GetWindowLongPtrW(control, GWL_STYLE);
      if ((style & SS_CENTERIMAGE) == 0)
        SetWindowLongPtrW(control, GWL_STYLE, style | SS_CENTERIMAGE);
    }
    move(id, x, y, width, row_height);
  }
  void field_row(const int label_id, const int control_id, const int x,
                 const int y, const int column_width, const int total_width,
                 const int control_height) const {
    row_label(label_id, x, y, column_width - scaled(5), control_height);
    move(control_id, x + column_width, y,
         std::max(1, total_width - column_width), control_height);
  }
  void next_button(const int id, int &x, const int y,
                   const int logical_width, const int height,
                   const int gap) const {
    move(id, x, y, scaled(logical_width), height);
    x += scaled(logical_width) + gap;
  }

  [[nodiscard]] HWND control(const int id) const noexcept {
    return GetDlgItem(dialog_, id);
  }
  [[nodiscard]] SIZE text_extent(const int id, const int wrap_width = 0) const {
    return control_text_extent(control(id), wrap_width);
  }
  // A dialog button is at least 88 px wide, wider when its label needs it.
  [[nodiscard]] int button_width(const int id) const {
    return std::max(scaled(88),
                    static_cast<int>(text_extent(id).cx) + scaled(24));
  }
  // A drop-down centered in its row: Windows sets its closed height from the
  // font, and its window height also holds the open list.
  void combo(const int id, const int x, const int y, const int width,
             const int row_height) const {
    const int list = scaled(200);
    move(id, x, y, width, list);
    RECT closed{};
    if (const HWND box = control(id);
        box != nullptr && GetWindowRect(box, &closed) != FALSE &&
        closed.bottom - closed.top < row_height)
      move(id, x, y + (row_height - (closed.bottom - closed.top)) / 2, width,
           list);
  }
  // A dialog's button row: the buttons in order, ending at `right`, 8 px
  // apart (OK, Cancel, Apply). Returns the left edge of the first.
  int button_row(const std::initializer_list<int> ids, const int right,
                 const int y, const int height) const {
    int x = right + scaled(8);
    for (const int id : ids)
      x -= button_width(id) + scaled(8);
    const int left = x;
    for (const int id : ids) {
      const int width = button_width(id);
      move(id, x, y, width, height);
      x += width + scaled(8);
    }
    return left;
  }
  // Sizes a fixed dialog so its client area is `width` by `height`.
  void fit_client(const int width, const int height) const {
    RECT frame{0, 0, width, height};
    AdjustWindowRectExForDpi(
        &frame, static_cast<DWORD>(GetWindowLongPtrW(dialog_, GWL_STYLE)),
        GetMenu(dialog_) != nullptr,
        static_cast<DWORD>(GetWindowLongPtrW(dialog_, GWL_EXSTYLE)),
        GetDpiForWindow(dialog_));
    SetWindowPos(dialog_, nullptr, 0, 0, frame.right - frame.left,
                 frame.bottom - frame.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }

private:
  HWND dialog_{};
  int dpi_{};
};

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
    const bool styled =
        dialog_ != nullptr && replace_dialog_font(dialog_, dpi, font_);
    apply_monospace(dpi);
    return styled;
  }

  [[nodiscard]] bool on_dpi_changed(const UINT dpi, const LPARAM lparam) {
    if (dialog_ == nullptr)
      return false;
    apply_suggested_dpi_rect(dialog_, lparam);
    const bool styled = replace_dialog_font(dialog_, dpi, font_);
    apply_monospace(dpi);
    return styled;
  }

  // JSON, traces and other machine text use the monospace font (Cascadia
  // Mono, else Consolas); restyling gives children the body font, so it is
  // re-applied after every font change.
  void set_monospace(const std::initializer_list<int> ids) {
    monospace_ids_.assign(ids.begin(), ids.end());
    if (dialog_ != nullptr)
      apply_monospace(GetDpiForWindow(dialog_));
  }

  void on_minimum_size(const LPARAM lparam, const int logical_width,
                       const int logical_height) const {
    if (dialog_ != nullptr)
      apply_minimum_track_size(dialog_, lparam, logical_width, logical_height);
  }

  void on_destroy() noexcept {
    destroy_dialog_font(font_);
    destroy_dialog_font(monospace_);
    dialog_ = nullptr;
  }

  [[nodiscard]] HWND window() const noexcept { return dialog_; }
  [[nodiscard]] HFONT font() const noexcept { return font_; }

private:
  void apply_monospace(const UINT dpi) {
    if (dialog_ == nullptr || monospace_ids_.empty())
      return;
    const HFONT replacement = layerc::platform::create_native_monospace_font(dpi);
    if (replacement == nullptr)
      return;
    for (const int id : monospace_ids_) {
      if (const HWND control = GetDlgItem(dialog_, id); control != nullptr)
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(replacement),
                     TRUE);
    }
    destroy_dialog_font(monospace_);
    monospace_ = replacement;
  }

  HWND dialog_{};
  HFONT font_{};
  HFONT monospace_{};
  std::vector<int> monospace_ids_;
};

} // namespace layerc::editor
