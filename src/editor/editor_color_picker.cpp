#include "editor_window.hpp"
#include "editor_dialog_dispatch.hpp"
#include "editor_resource.h"
#include "layerc/platform/native_theme.hpp"

#include <array>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

namespace layerc::editor {
namespace {

namespace theme = layerc::platform;

struct PaletteColor {
  std::uint32_t value;
  std::wstring_view name;
};

constexpr std::array<PaletteColor, 24> color_palette{
    PaletteColor{theme::copper_theme.background, L"Graphite"},
    PaletteColor{theme::copper_theme.panel, L"Charcoal"},
    PaletteColor{theme::copper_theme.border, L"Slate"},
    PaletteColor{theme::copper_theme.muted, L"Silver"},
    PaletteColor{theme::copper_theme.text, L"Warm white"},
    PaletteColor{0xF1F5F7, L"Ice white"},
    PaletteColor{theme::copper_theme.accent, L"Copper"},
    PaletteColor{0xE0A15A, L"Amber"},
    PaletteColor{0xE85D5D, L"Red"},
    PaletteColor{0xF08A5D, L"Coral"},
    PaletteColor{0xE7C84B, L"Gold"},
    PaletteColor{0x8BC34A, L"Lime"},
    PaletteColor{0x4FCB83, L"Green"},
    PaletteColor{theme::teal_theme.accent, L"Seafoam"},
    PaletteColor{0x50BAC8, L"Teal"},
    PaletteColor{0x5DADE2, L"Sky blue"},
    PaletteColor{0x4B7BEC, L"Blue"},
    PaletteColor{0x6C5CE7, L"Indigo"},
    PaletteColor{0x9B72E8, L"Violet"},
    PaletteColor{theme::plum_theme.accent, L"Plum"},
    PaletteColor{0xD65DB1, L"Magenta"},
    PaletteColor{0xF06292, L"Pink"},
    PaletteColor{0x8D6E63, L"Brown"},
    PaletteColor{0x607D8B, L"Blue gray"},
};

std::optional<std::uint32_t> channel_value(const HWND window, const int id) {
  return parse_bounded(control_text(GetDlgItem(window, id)), 0, 255);
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
  if (!text.empty() && text.front() == L'#') text.erase(text.begin());
  if (text.size() != 6) return std::nullopt;
  return parse_bounded(text, 0, 0xFFFFFFU, 16);
}

std::wstring hex_label(const std::uint32_t color) {
  std::array<wchar_t, 8> buffer{};
  std::swprintf(buffer.data(), buffer.size(), L"#%06X", color & 0xFFFFFFU);
  return buffer.data();
}

std::wstring swatch_label(const PaletteColor &color) {
  return std::wstring(color.name) + L", " + hex_label(color.value);
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
      shell_.instance(), MAKEINTRESOURCEW(IDD_COLOR_PICKER), owner,
      &dispatch_member_dialog<EditorWindow,
                              &EditorWindow::initialize_color_picker,
                              &EditorWindow::handle_color_picker_message>,
      reinterpret_cast<LPARAM>(this));
  if (result != IDOK) return false;
  value = appearance_editor_.picker_color();
  return true;
}

void EditorWindow::initialize_color_picker(const HWND window) {
  apply_dialog_chrome(window, shell_.capture_exclusion());
  style_dialog_children(window);
  for (int id = IDC_COLOR_SWATCH_FIRST; id <= IDC_COLOR_SWATCH_LAST; ++id)
    set_text(GetDlgItem(window, id), swatch_label(
             color_palette[static_cast<std::size_t>(id - IDC_COLOR_SWATCH_FIRST)]));
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
  const auto value = hex_value(control_text(GetDlgItem(window, IDC_COLOR_HEX)));
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
  fill_solid_rect(draw.hDC, sample, color_ref(*color));
  outline_rect(draw.hDC, sample, selected ? 2 : 1,
               selected ? role_colors.selection : role_colors.border);
  if ((draw.itemState & ODS_FOCUS) != 0) {
    RECT focus = draw.rcItem;
    InflateRect(&focus, -1, -1);
    frame_solid_rect(draw.hDC, focus, role_colors.text);
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
      appearance_editor_.set_picker_color(
          color_palette[static_cast<std::size_t>(id - IDC_COLOR_SWATCH_FIRST)].value);
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
