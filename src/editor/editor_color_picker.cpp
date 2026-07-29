#include "editor_window.hpp"
#include "editor_resource.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <optional>
#include <string>

#include <windows.h>

namespace layerc::editor {
namespace {

constexpr std::array<std::uint32_t, 24> color_palette{
    0x1B1F24, 0x282D34, 0x4B525C, 0x9CA3AD, 0xE9E4DD, 0xF1F5F7,
    0xD88E4B, 0xE0A15A, 0xE85D5D, 0xF08A5D, 0xE7C84B, 0x8BC34A,
    0x4FCB83, 0x5AB6AA, 0x50BAC8, 0x5DADE2, 0x4B7BEC, 0x6C5CE7,
    0x9B72E8, 0xB382E2, 0xD65DB1, 0xF06292, 0x8D6E63, 0x607D8B,
};

std::optional<std::uint32_t> channel_value(const HWND window, const int id) {
  const auto text = control_text(GetDlgItem(window, id));
  if (text.empty()) return std::nullopt;
  errno = 0;
  wchar_t *end{};
  const unsigned long value = std::wcstoul(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != L'\0' || value > 255)
    return std::nullopt;
  return static_cast<std::uint32_t>(value);
}

std::optional<std::uint32_t> rgb_fields(const HWND window) {
  const auto red = channel_value(window, IDC_COLOR_RED);
  const auto green = channel_value(window, IDC_COLOR_GREEN);
  const auto blue = channel_value(window, IDC_COLOR_BLUE);
  if (!red.has_value() || !green.has_value() || !blue.has_value())
    return std::nullopt;
  return (*red << 16U) | (*green << 8U) | *blue;
}

int hex_digit(const wchar_t character) {
  if (character >= L'0' && character <= L'9') return character - L'0';
  if (character >= L'a' && character <= L'f') return character - L'a' + 10;
  if (character >= L'A' && character <= L'F') return character - L'A' + 10;
  return -1;
}

std::optional<std::uint32_t> hex_value(std::wstring text) {
  if (!text.empty() && text.front() == L'#') text.erase(text.begin());
  if (text.size() != 6) return std::nullopt;
  std::uint32_t value{};
  for (const wchar_t character : text) {
    const int digit = hex_digit(character);
    if (digit < 0) return std::nullopt;
    value = (value << 4U) | static_cast<std::uint32_t>(digit);
  }
  return value;
}

std::wstring hex_label(const std::uint32_t color) {
  std::array<wchar_t, 8> buffer{};
  std::swprintf(buffer.data(), buffer.size(), L"#%06X", color & 0xFFFFFFU);
  return buffer.data();
}

void invalidate_picker(const HWND window) {
  for (int id = IDC_COLOR_SWATCH_FIRST; id <= IDC_COLOR_SWATCH_LAST; ++id)
    InvalidateRect(GetDlgItem(window, id), nullptr, TRUE);
  InvalidateRect(GetDlgItem(window, IDC_COLOR_PREVIEW), nullptr, TRUE);
}

} // namespace

INT_PTR CALLBACK EditorWindow::color_dialog_proc(const HWND window,
                                                  const UINT message,
                                                  const WPARAM wparam,
                                                  const LPARAM lparam) {
  auto *editor = reinterpret_cast<EditorWindow *>(
      GetWindowLongPtrW(window, DWLP_USER));
  if (message == WM_INITDIALOG) {
    editor = reinterpret_cast<EditorWindow *>(lparam);
    SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(editor));
    editor->initialize_color_picker(window);
    return TRUE;
  }
  return editor == nullptr
             ? FALSE
             : editor->handle_color_picker_message(window, message, wparam,
                                                   lparam);
}

bool EditorWindow::choose_color(const HWND owner, std::uint32_t &value) {
  picker_color_ = value & 0xFFFFFFU;
  const INT_PTR result = DialogBoxParamW(
      instance_, MAKEINTRESOURCEW(IDD_COLOR_PICKER), owner,
      &EditorWindow::color_dialog_proc, reinterpret_cast<LPARAM>(this));
  if (result != IDOK) return false;
  value = picker_color_;
  return true;
}

void EditorWindow::initialize_color_picker(const HWND window) {
  set_dark_title_bar(window);
  style_dialog_children(window);
  (void)layerc::platform::apply_capture_exclusion(window, capture_exclusion_);
  refresh_color_picker(window);
}

void EditorWindow::refresh_color_picker(const HWND window) {
  picker_syncing_ = true;
  set_text(GetDlgItem(window, IDC_COLOR_RED),
           std::to_wstring((picker_color_ >> 16U) & 0xFFU));
  set_text(GetDlgItem(window, IDC_COLOR_GREEN),
           std::to_wstring((picker_color_ >> 8U) & 0xFFU));
  set_text(GetDlgItem(window, IDC_COLOR_BLUE),
           std::to_wstring(picker_color_ & 0xFFU));
  set_text(GetDlgItem(window, IDC_COLOR_HEX), hex_label(picker_color_));
  picker_syncing_ = false;
  invalidate_picker(window);
}

void EditorWindow::update_color_picker_from_rgb(const HWND window) {
  if (picker_syncing_) return;
  const auto value = rgb_fields(window);
  if (!value.has_value()) return;
  picker_color_ = *value;
  picker_syncing_ = true;
  set_text(GetDlgItem(window, IDC_COLOR_HEX), hex_label(picker_color_));
  picker_syncing_ = false;
  invalidate_picker(window);
}

void EditorWindow::update_color_picker_from_hex(const HWND window) {
  if (picker_syncing_) return;
  const auto value = hex_value(control_text(GetDlgItem(window, IDC_COLOR_HEX)));
  if (!value.has_value()) return;
  picker_color_ = *value;
  picker_syncing_ = true;
  set_text(GetDlgItem(window, IDC_COLOR_RED),
           std::to_wstring((picker_color_ >> 16U) & 0xFFU));
  set_text(GetDlgItem(window, IDC_COLOR_GREEN),
           std::to_wstring((picker_color_ >> 8U) & 0xFFU));
  set_text(GetDlgItem(window, IDC_COLOR_BLUE),
           std::to_wstring(picker_color_ & 0xFFU));
  picker_syncing_ = false;
  invalidate_picker(window);
}

bool EditorWindow::draw_color_picker_item(const DRAWITEMSTRUCT &draw) const {
  const int id = static_cast<int>(draw.CtlID);
  std::optional<std::uint32_t> color;
  bool selected{};
  if (id >= IDC_COLOR_SWATCH_FIRST && id <= IDC_COLOR_SWATCH_LAST) {
    color = color_palette[static_cast<std::size_t>(id - IDC_COLOR_SWATCH_FIRST)];
    selected = *color == picker_color_;
  } else if (id == IDC_COLOR_PREVIEW) {
    color = picker_color_;
    selected = true;
  } else {
    return false;
  }

  const auto role_colors = colors(layerc::platform::EditorControlRole::button);
  const HBRUSH background = CreateSolidBrush(role_colors.fill);
  FillRect(draw.hDC, &draw.rcItem, background);
  DeleteObject(background);
  RECT sample = draw.rcItem;
  InflateRect(&sample, -3, -3);
  const HBRUSH sample_brush = CreateSolidBrush(color_ref(*color));
  FillRect(draw.hDC, &sample, sample_brush);
  DeleteObject(sample_brush);
  const HPEN border = CreatePen(PS_SOLID, selected ? 2 : 1,
                               selected ? role_colors.selection
                                        : role_colors.border);
  const HGDIOBJ previous_pen = SelectObject(draw.hDC, border);
  const HGDIOBJ previous_brush =
      SelectObject(draw.hDC, GetStockObject(NULL_BRUSH));
  Rectangle(draw.hDC, sample.left, sample.top, sample.right, sample.bottom);
  SelectObject(draw.hDC, previous_brush);
  SelectObject(draw.hDC, previous_pen);
  DeleteObject(border);
  if ((draw.itemState & ODS_FOCUS) != 0) {
    RECT focus = draw.rcItem;
    InflateRect(&focus, -1, -1);
    const HBRUSH focus_brush = CreateSolidBrush(role_colors.text);
    FrameRect(draw.hDC, &focus, focus_brush);
    DeleteObject(focus_brush);
  }
  return true;
}

LRESULT EditorWindow::handle_color_picker_message(const HWND window,
                                                   const UINT message,
                                                   const WPARAM wparam,
                                                   const LPARAM lparam) {
  switch (message) {
  case WM_COMMAND: {
    const int id = LOWORD(wparam);
    const int notification = HIWORD(wparam);
    if (id >= IDC_COLOR_SWATCH_FIRST && id <= IDC_COLOR_SWATCH_LAST &&
        notification == BN_CLICKED) {
      picker_color_ =
          color_palette[static_cast<std::size_t>(id - IDC_COLOR_SWATCH_FIRST)];
      refresh_color_picker(window);
      return TRUE;
    }
    if (notification == EN_CHANGE) {
      if (id == IDC_COLOR_RED || id == IDC_COLOR_GREEN ||
          id == IDC_COLOR_BLUE) {
        update_color_picker_from_rgb(window);
        return TRUE;
      }
      if (id == IDC_COLOR_HEX) {
        update_color_picker_from_hex(window);
        return TRUE;
      }
    }
    if (id == IDOK) {
      const auto rgb = rgb_fields(window);
      const auto hex = hex_value(control_text(GetDlgItem(window, IDC_COLOR_HEX)));
      if (!rgb.has_value() || !hex.has_value() || *rgb != *hex) {
        MessageBoxW(window, L"Enter RGB values from 0 to 255 or a #RRGGBB value.",
                    L"Invalid color", MB_OK | MB_ICONWARNING);
        return TRUE;
      }
      picker_color_ = *rgb;
      EndDialog(window, IDOK);
      return TRUE;
    }
    if (id == IDCANCEL) {
      EndDialog(window, IDCANCEL);
      return TRUE;
    }
    break;
  }
  case WM_DRAWITEM: {
    const auto *draw = reinterpret_cast<const DRAWITEMSTRUCT *>(lparam);
    if (draw_color_picker_item(*draw)) return TRUE;
    if (draw->CtlType == ODT_BUTTON) {
      draw_button(*draw, layerc::platform::EditorControlRole::button);
      return TRUE;
    }
    break;
  }
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
    return color_message(message, wparam, lparam);
  case WM_ERASEBKGND: {
    RECT client{};
    GetClientRect(window, &client);
    FillRect(reinterpret_cast<HDC>(wparam), &client,
             brush(layerc::platform::EditorControlRole::window));
    return TRUE;
  }
  case WM_CLOSE:
    EndDialog(window, IDCANCEL);
    return TRUE;
  default:
    break;
  }
  return FALSE;
}

} // namespace layerc::editor