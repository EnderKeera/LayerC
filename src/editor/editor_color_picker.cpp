#include "editor_window.hpp"
#include "accessible_text.hpp"
#include "appearance_contrast.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_dialog_layouts.hpp"
#include "editor_resource.h"
#include "static_style.hpp"
#include "layerc/core/json.hpp"
#include "layerc/platform/native_theme.hpp"
#include "layerc/platform/text.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

namespace layerc::editor {
namespace {

namespace theme = layerc::platform;

constexpr const auto &color_palette = theme::color_palette;



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

// The hint and the contrast line belong to the value fields: the line
// measures the color being chosen. Swatches describe their own colors
// (describe_swatches), so selection never changes them.
void describe_value_fields(const HWND window) {
  const std::wstring hint = window_text(GetDlgItem(window, IDC_COLOR_HINT));
  const std::wstring contrast =
      window_text(GetDlgItem(window, IDC_COLOR_CONTRAST));
  std::wstring fields(hint);
  if (!contrast.empty()) {
    if (!fields.empty())
      fields += L' ';
    fields += contrast;
  }
  for (const int id :
       {IDC_COLOR_RED, IDC_COLOR_GREEN, IDC_COLOR_BLUE, IDC_COLOR_HEX})
    set_accessible_description(GetDlgItem(window, id), fields);
}

// A swatch's description is its own color's contrast with the picker's
// targets (DesignSystem.md, Screen readers). Swatch colors and targets are
// fixed while the picker is open, so this runs once.
void describe_swatches(const HWND window,
                       const std::vector<ContrastTarget> &targets) {
  const auto descriptions = palette_swatch_descriptions(targets);
  for (int id = IDC_COLOR_SWATCH_FIRST; id <= IDC_COLOR_SWATCH_LAST; ++id) {
    const auto slot = static_cast<std::size_t>(id - IDC_COLOR_SWATCH_FIRST);
    set_accessible_description(GetDlgItem(window, id),
                               slot < descriptions.size() ? descriptions[slot]
                                                          : std::wstring{});
  }
}

// Repaints the swatches and the preview, and rewrites the contrast line
// under the preview for `color` (DesignSystem.md, Roles).
void invalidate_picker(const HWND window, const std::uint32_t color,
                       const std::vector<ContrastTarget> &targets) {
  for (int id = IDC_COLOR_SWATCH_FIRST; id <= IDC_COLOR_SWATCH_LAST; ++id)
    InvalidateRect(GetDlgItem(window, id), nullptr, TRUE);
  InvalidateRect(GetDlgItem(window, IDC_COLOR_PREVIEW), nullptr, TRUE);
  const auto line = picker_contrast_line(native_color_ref(color), targets);
  std::optional<theme::StatusGlyph> glyph;
  if (line && line->below_minimum)
    glyph = theme::StatusGlyph::warning;
  set_status_line(GetDlgItem(window, IDC_COLOR_CONTRAST), glyph,
                  line ? std::wstring_view(line->text) : std::wstring_view{});
  describe_value_fields(window);
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
  set_text_tone(GetDlgItem(window, IDC_COLOR_HINT), TextTone::subtext);
  layout_color_picker_dialog(window);
  center_over_owner(window);
  int active = IDC_COLOR_SWATCH_FIRST;
  for (int id = IDC_COLOR_SWATCH_FIRST; id <= IDC_COLOR_SWATCH_LAST; ++id) {
    const auto &color =
        color_palette[static_cast<std::size_t>(id - IDC_COLOR_SWATCH_FIRST)];
    // The swatch paints its color and no words, so its text is only the
    // name read out (DesignSystem.md, Screen readers).
    set_text(GetDlgItem(window, id),
             swatch_name(color.name, hex_label(color.value),
                         id - IDC_COLOR_SWATCH_FIRST + 1, swatch_count));
    SetWindowSubclass(GetDlgItem(window, id), &swatch_proc, 1, 0);
    if (color.value == appearance_editor_.picker_color())
      active = id;
  }
  set_swatch_tab_stop(window, active);
  describe_swatches(window, appearance_editor_.picker_targets());
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
  invalidate_picker(window, appearance_editor_.picker_color(),
                    appearance_editor_.picker_targets());
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
  invalidate_picker(window, appearance_editor_.picker_color(),
                    appearance_editor_.picker_targets());
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
  invalidate_picker(window, appearance_editor_.picker_color(),
                    appearance_editor_.picker_targets());
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
        themed_message_box(
            window,
            L"Could not use this color: a value is missing or out of range. "
            L"Enter red, green and blue from 0 to 255, or a hex #RRGGBB value.",
            L"Choose color", MB_OK | MB_ICONWARNING);
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
