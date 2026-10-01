#include "editor_window.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_resource.h"
#include "layerc/core/json.hpp"
#include "layerc/platform/native_theme.hpp"
#include "layerc/platform/text.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

namespace layerc::editor {
namespace {

namespace theme = layerc::platform;

struct PaletteColor {
  std::uint32_t value;
  std::wstring_view name;
};

constexpr std::array<PaletteColor, 24> color_palette{
    PaletteColor{theme::neutral_theme.background, L"Graphite"},
    PaletteColor{theme::neutral_theme.panel, L"Charcoal"},
    PaletteColor{theme::neutral_theme.border, L"Gray"},
    PaletteColor{theme::neutral_theme.secondary, L"Silver"},
    PaletteColor{0xE9E4DD, L"Warm white"},
    PaletteColor{0xF1F5F7, L"Ice white"},
    PaletteColor{theme::copper_accent, L"Copper"},
    PaletteColor{0xE0A15A, L"Amber"},
    PaletteColor{0xE85D5D, L"Red"},
    PaletteColor{0xF08A5D, L"Coral"},
    PaletteColor{0xE7C84B, L"Gold"},
    PaletteColor{0x8BC34A, L"Lime"},
    PaletteColor{0x4FCB83, L"Green"},
    PaletteColor{theme::teal_accent, L"Seafoam"},
    PaletteColor{0x50BAC8, L"Teal"},
    PaletteColor{0x5DADE2, L"Sky blue"},
    PaletteColor{0x4B7BEC, L"Blue"},
    PaletteColor{0x6C5CE7, L"Indigo"},
    PaletteColor{0x9B72E8, L"Violet"},
    PaletteColor{theme::plum_accent, L"Plum"},
    PaletteColor{0xD65DB1, L"Magenta"},
    PaletteColor{0xF06292, L"Pink"},
    PaletteColor{0x8D6E63, L"Brown"},
    PaletteColor{0x607D8B, L"Blue gray"},
};

std::optional<std::uint32_t> channel_value(const HWND window, const int id) {
  return parse_bounded(window_text(GetDlgItem(window, id)), 0, 255);
}

std::optional<std::uint32_t> rgb_fields(const HWND window) {
  const auto red = channel_value(window, IDC_COLOR_RED);
  const auto green = channel_value(window, IDC_COLOR_GREEN);
  const auto blue = channel_value(window, IDC_COLOR_BLUE);
  if (!red.has_value() || !green.has_value() || !blue.has_value())
    return std::nullopt;
  return (*red << 16U) | (*green << 8U) | *blue;
}

std::optional<std::uint32_t> hex_value(std::wstring text) {
  if (text.empty() || text.front() != L'#') text.insert(text.begin(), L'#');
  const auto narrow = layerc::platform::ascii_narrow(text);
  return narrow ? layerc::core::json::parse_color_text(*narrow) : std::nullopt;
}

std::wstring hex_label(const std::uint32_t color) {
  return layerc::platform::utf8_to_wide(layerc::core::json::color_text(color));
}

std::wstring swatch_label(const PaletteColor &color) {
  return std::wstring(color.name) + L", " + hex_label(color.value);
}

// The swatch grid is one tab stop (the selected or focused swatch) navigated
// with the arrow keys; moving selects, like a radio group.
constexpr int swatch_columns = 8;
constexpr int swatch_count = IDC_COLOR_SWATCH_LAST - IDC_COLOR_SWATCH_FIRST + 1;

void set_swatch_tab_stop(const HWND window, const int active_id) {
  for (int id = IDC_COLOR_SWATCH_FIRST; id <= IDC_COLOR_SWATCH_LAST; ++id) {
    const HWND swatch = GetDlgItem(window, id);
    auto style = GetWindowLongPtrW(swatch, GWL_STYLE);
    style = id == active_id ? (style | WS_TABSTOP) : (style & ~WS_TABSTOP);
    SetWindowLongPtrW(swatch, GWL_STYLE, style);
  }
}

LRESULT CALLBACK swatch_proc(const HWND swatch, const UINT message,
                             const WPARAM wparam, const LPARAM lparam,
                             const UINT_PTR, const DWORD_PTR) {
  if (message == WM_GETDLGCODE)
    return DefSubclassProc(swatch, message, wparam, lparam) | DLGC_WANTARROWS;
  if (message == WM_KEYDOWN) {
    const int index = GetDlgCtrlID(swatch) - IDC_COLOR_SWATCH_FIRST;
    int next = index;
    switch (wparam) {
    case VK_LEFT: next = index - 1; break;
    case VK_RIGHT: next = index + 1; break;
    case VK_UP: next = index - swatch_columns; break;
    case VK_DOWN: next = index + swatch_columns; break;
    case VK_HOME: next = 0; break;
    case VK_END: next = swatch_count - 1; break;
    default: return DefSubclassProc(swatch, message, wparam, lparam);
    }
    if (next < 0 || next >= swatch_count)
      return 0;
    const HWND dialog = GetParent(swatch);
    const int id = IDC_COLOR_SWATCH_FIRST + next;
    set_swatch_tab_stop(dialog, id);
    SetFocus(GetDlgItem(dialog, id));
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
                 reinterpret_cast<LPARAM>(GetDlgItem(dialog, id)));
    return 0;
  }
  if (message == WM_NCDESTROY)
    RemoveWindowSubclass(swatch, &swatch_proc, 1);
  return DefSubclassProc(swatch, message, wparam, lparam);
}

void invalidate_picker(const HWND window) {
  for (int id = IDC_COLOR_SWATCH_FIRST; id <= IDC_COLOR_SWATCH_LAST; ++id)
    InvalidateRect(GetDlgItem(window, id), nullptr, TRUE);
  InvalidateRect(GetDlgItem(window, IDC_COLOR_PREVIEW), nullptr, TRUE);
}

} // namespace

bool EditorWindow::choose_color(const HWND owner, std::uint32_t &value) {
  appearance_editor_.set_picker_color(value);
  const INT_PTR result = DialogBoxParamW(
      shell_.instance, MAKEINTRESOURCEW(IDD_COLOR_PICKER), owner,
      &dispatch_member_dialog<EditorWindow,
                              &EditorWindow::initialize_color_picker,
                              &EditorWindow::handle_color_picker_message>,
      reinterpret_cast<LPARAM>(this));
  if (result != IDOK) return false;
  value = appearance_editor_.picker_color();
  return true;
}

void EditorWindow::initialize_color_picker(const HWND window) {
  apply_dialog_chrome(window, shell_.capture_exclusion);
  style_dialog_children(window);
  int active = IDC_COLOR_SWATCH_FIRST;
  for (int id = IDC_COLOR_SWATCH_FIRST; id <= IDC_COLOR_SWATCH_LAST; ++id) {
    const auto &color =
        color_palette[static_cast<std::size_t>(id - IDC_COLOR_SWATCH_FIRST)];
    set_text(GetDlgItem(window, id), swatch_label(color));
    SetWindowSubclass(GetDlgItem(window, id), &swatch_proc, 1, 0);
    if (color.value == appearance_editor_.picker_color())
      active = id;
  }
  set_swatch_tab_stop(window, active);
  SendMessageW(GetDlgItem(window, IDC_COLOR_HEX), EM_SETCUEBANNER, FALSE,
               reinterpret_cast<LPARAM>(L"#RRGGBB"));
  refresh_color_picker(window);
}

void EditorWindow::refresh_color_picker(const HWND window) {
  {
    auto sync = appearance_editor_.picker_sync_scope();
    refresh_color_picker_rgb(window);
    set_text(GetDlgItem(window, IDC_COLOR_HEX), hex_label(appearance_editor_.picker_color()));
  }
  invalidate_picker(window);
}

void EditorWindow::refresh_color_picker_rgb(const HWND window) {
  set_text(GetDlgItem(window, IDC_COLOR_RED),
           std::to_wstring((appearance_editor_.picker_color() >> 16U) & 0xFFU));
  set_text(GetDlgItem(window, IDC_COLOR_GREEN),
           std::to_wstring((appearance_editor_.picker_color() >> 8U) & 0xFFU));
  set_text(GetDlgItem(window, IDC_COLOR_BLUE),
           std::to_wstring(appearance_editor_.picker_color() & 0xFFU));
}

void EditorWindow::update_color_picker_from_rgb(const HWND window) {
  if (appearance_editor_.picker_syncing()) return;
  const auto value = rgb_fields(window);
  if (!value.has_value()) return;
  appearance_editor_.set_picker_color(*value);
  {
    auto sync = appearance_editor_.picker_sync_scope();
    set_text(GetDlgItem(window, IDC_COLOR_HEX), hex_label(appearance_editor_.picker_color()));
  }
  invalidate_picker(window);
}

void EditorWindow::update_color_picker_from_hex(const HWND window) {
  if (appearance_editor_.picker_syncing()) return;
  const auto value = hex_value(window_text(GetDlgItem(window, IDC_COLOR_HEX)));
  if (!value.has_value()) return;
  appearance_editor_.set_picker_color(*value);
  {
    auto sync = appearance_editor_.picker_sync_scope();
    refresh_color_picker_rgb(window);
  }
  invalidate_picker(window);
}

bool EditorWindow::draw_color_picker_item(const DRAWITEMSTRUCT &draw) const {
  const int id = static_cast<int>(draw.CtlID);
  std::optional<std::uint32_t> color;
  bool selected{};
  if (id >= IDC_COLOR_SWATCH_FIRST && id <= IDC_COLOR_SWATCH_LAST) {
    color = color_palette[static_cast<std::size_t>(id - IDC_COLOR_SWATCH_FIRST)].value;
    selected = *color == appearance_editor_.picker_color();
  } else if (id == IDC_COLOR_PREVIEW) {
    color = appearance_editor_.picker_color();
    selected = true;
  } else {
    return false;
  }

  const auto role_colors = colors(layerc::platform::EditorControlRole::button);
  fill_solid_rect(draw.hDC, draw.rcItem, role_colors.fill);
  RECT sample = draw.rcItem;
  InflateRect(&sample, -3, -3);
  fill_solid_rect(draw.hDC, sample, native_color_ref(*color));
  outline_rect(draw.hDC, sample, selected ? 2 : 1,
               selected ? role_colors.selection : role_colors.border);
  // The shared accent focus ring every other control uses.
  if ((draw.itemState & ODS_FOCUS) != 0)
    layerc::platform::paint_focus_ring(draw.hDC, draw.rcItem,
                                       role_colors.accent,
                                       GetDpiForWindow(draw.hwndItem));
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
      appearance_editor_.set_picker_color(
          color_palette[static_cast<std::size_t>(id - IDC_COLOR_SWATCH_FIRST)].value);
      set_swatch_tab_stop(window, id);
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
      const auto hex = hex_value(window_text(GetDlgItem(window, IDC_COLOR_HEX)));
      if (!rgb.has_value() || !hex.has_value() || *rgb != *hex) {
        themed_message_box(window, L"Enter RGB values from 0 to 255 or a #RRGGBB value.",
                    L"Invalid color", MB_OK | MB_ICONWARNING);
        return TRUE;
      }
      appearance_editor_.set_picker_color(*rgb);
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
  case WM_CLOSE:
    EndDialog(window, IDCANCEL);
    return TRUE;
  default:
    break;
  }
  return dialog_theme_message(window, message, wparam, lparam).value_or(FALSE);
}

} // namespace layerc::editor
