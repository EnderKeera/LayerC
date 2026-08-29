#include "layerc/platform/native_theme.hpp"

#include <algorithm>
#include <array>
#include <commctrl.h>
#include <cstring>
#include <cwchar>
#include <dwmapi.h>
#include <uxtheme.h>

namespace layerc::platform {
namespace {

constexpr int dark_title_bar_attribute = 20;

enum class PreferredAppMode : int {
  default_mode,
  allow_dark,
  force_dark,
  force_light,
  maximum,
};

template <typename Function>
Function uxtheme_function(const HMODULE module, const LPCSTR ordinal) {
  const FARPROC procedure = GetProcAddress(module, ordinal);
  static_assert(sizeof(Function) == sizeof(procedure));
  Function function{};
  std::memcpy(&function, &procedure, sizeof(function));
  return function;
}

HMODULE uxtheme_module() {
  static const HMODULE module = [] {
    const HMODULE loaded =
        LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return loaded != nullptr ? loaded : LoadLibraryW(L"uxtheme.dll");
  }();
  return module;
}

ThemeBase theme_base(const EditorTheme theme) {
  switch (theme) {
  case EditorTheme::system: {
    auto value = dark_theme;
    value.accent = native_rgb_value(GetSysColor(COLOR_HIGHLIGHT));
    return value;
  }
  case EditorTheme::dark:
    return dark_theme;
  case EditorTheme::teal:
    return teal_theme;
  case EditorTheme::plum:
    return plum_theme;
  case EditorTheme::copper:
  default:
    return copper_theme;
  }
}

NativeThemeColors default_theme_colors(const EditorTheme theme,
                                       const std::uint32_t accent,
                                       const EditorControlRole role) {
  const auto base = theme_base(theme);
  const auto accent_color = accent == 0 ? base.accent : accent;
  const auto accent_dark =
      ((accent_color & 0xFF0000U) * 55U / 100U & 0xFF0000U) |
      ((accent_color & 0x00FF00U) * 55U / 100U & 0x00FF00U) |
      ((accent_color & 0x0000FFU) * 55U / 100U & 0x0000FFU);
  NativeThemeColors colors{.fill = native_color_ref(base.background),
                           .text = native_color_ref(base.text),
                           .border = native_color_ref(base.border),
                           .hover = native_color_ref(base.panel),
                           .selection = native_color_ref(accent_dark),
                           .disabled = native_color_ref(base.muted)};
  switch (role) {
  case EditorControlRole::toolbar:
    colors.fill = native_color_ref(base.background);
    break;
  case EditorControlRole::sidebar:
    colors.fill = native_color_ref(base.panel);
    break;
  case EditorControlRole::label:
  case EditorControlRole::status:
    colors.text = native_color_ref(base.muted);
    break;
  case EditorControlRole::button:
  case EditorControlRole::checkbox:
    colors.fill = native_color_ref(base.panel);
    colors.hover = native_color_ref(base.input);
    break;
  case EditorControlRole::edit:
  case EditorControlRole::list:
  case EditorControlRole::preview:
    colors.fill = native_color_ref(base.input);
    break;
  case EditorControlRole::window:
  case EditorControlRole::count:
    break;
  }
  return colors;
}

} // namespace

COLORREF native_color_ref(const std::uint32_t color) {
  return RGB((color >> 16U) & 0xFFU, (color >> 8U) & 0xFFU, color & 0xFFU);
}

std::uint32_t native_rgb_value(const COLORREF color) {
  return (static_cast<std::uint32_t>(GetRValue(color)) << 16U) |
         (static_cast<std::uint32_t>(GetGValue(color)) << 8U) |
         static_cast<std::uint32_t>(GetBValue(color));
}

NativeThemeColors native_theme_colors(
    const EditorTheme theme, const std::uint32_t accent,
    const EditorControlRole role,
    const EditorAppearanceOverrides &overrides) {
  auto colors = default_theme_colors(theme, accent, role);
  if ((overrides.fields & editor_fill) != 0)
    colors.fill = native_color_ref(overrides.fill);
  if ((overrides.fields & editor_text) != 0)
    colors.text = native_color_ref(overrides.text);
  if ((overrides.fields & editor_border) != 0)
    colors.border = native_color_ref(overrides.border);
  if ((overrides.fields & editor_hover) != 0)
    colors.hover = native_color_ref(overrides.hover);
  if ((overrides.fields & editor_selection) != 0)
    colors.selection = native_color_ref(overrides.selection);
  if ((overrides.fields & editor_disabled) != 0)
    colors.disabled = native_color_ref(overrides.disabled);
  return colors;
}

NativeThemeColors native_theme_colors(const EditorTheme theme,
                                      const std::uint32_t accent,
                                      const EditorControlRole role) {
  return native_theme_colors(theme, accent, role,
                             editor_appearance_overrides(role));
}

void enable_native_dark_mode() {
  const HMODULE module = uxtheme_module();
  if (module == nullptr)
    return;
  using SetPreferredAppMode = PreferredAppMode(WINAPI *)(PreferredAppMode);
  using FlushMenuThemes = void(WINAPI *)();
  if (const auto set_mode = uxtheme_function<SetPreferredAppMode>(
          module, MAKEINTRESOURCEA(135));
      set_mode != nullptr)
    (void)set_mode(PreferredAppMode::allow_dark);
  if (const auto flush =
          uxtheme_function<FlushMenuThemes>(module, MAKEINTRESOURCEA(136));
      flush != nullptr)
    flush();
}

void set_dark_title_bar(const HWND window) {
  if (const HMODULE module = uxtheme_module(); module != nullptr) {
    using AllowDarkModeForWindow = BOOL(WINAPI *)(HWND, BOOL);
    if (const auto allow = uxtheme_function<AllowDarkModeForWindow>(
            module, MAKEINTRESOURCEA(133));
        allow != nullptr)
      (void)allow(window, TRUE);
  }
  SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
  const BOOL enabled = TRUE;
  (void)DwmSetWindowAttribute(window, dark_title_bar_attribute, &enabled,
                              sizeof(enabled));
}

void style_native_dialog_children(const HWND dialog, const HFONT font,
                                  const bool owner_draw_list_boxes) {
  for (HWND child = GetWindow(dialog, GW_CHILD); child != nullptr;
       child = GetWindow(child, GW_HWNDNEXT)) {
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    std::array<wchar_t, 64> class_name{};
    GetClassNameW(child, class_name.data(),
                  static_cast<int>(class_name.size()));
    if (wcscmp(class_name.data(), L"Button") == 0) {
      const auto style = GetWindowLongPtrW(child, GWL_STYLE);
      const auto button_type = style & static_cast<LONG_PTR>(BS_TYPEMASK);
      if (button_type == BS_PUSHBUTTON) {
        SetWindowLongPtrW(child, GWL_STYLE,
                          (style & ~static_cast<LONG_PTR>(BS_TYPEMASK)) |
                              BS_OWNERDRAW);
        SetWindowPos(child, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                         SWP_FRAMECHANGED);
      } else {
        SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
      }
    } else if (wcscmp(class_name.data(), L"ComboBox") == 0) {
      SetWindowTheme(child, L"DarkMode_CFD", nullptr);
    } else if (wcscmp(class_name.data(), L"ListBox") == 0) {
      if (owner_draw_list_boxes) {
        const auto style = GetWindowLongPtrW(child, GWL_STYLE);
        SetWindowLongPtrW(child, GWL_STYLE,
                          style | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS);
        const UINT dpi = GetDpiForWindow(dialog);
        SendMessageW(child, LB_SETITEMHEIGHT, 0,
                     static_cast<LPARAM>(MulDiv(
                         18, dpi == 0 ? 96 : static_cast<int>(dpi), 96)));
      }
      SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
    } else if (wcscmp(class_name.data(), WC_LISTVIEWW) == 0) {
      SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
      if (const HWND header = ListView_GetHeader(child); header != nullptr)
        SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
      if (const HWND tooltip = ListView_GetToolTips(child); tooltip != nullptr)
        SetWindowTheme(tooltip, L"DarkMode_Explorer", nullptr);
    } else if (wcscmp(class_name.data(), L"Edit") == 0 ||
               wcscmp(class_name.data(), L"ScrollBar") == 0 ||
               wcscmp(class_name.data(), TOOLTIPS_CLASSW) == 0) {
      SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
    }
  }
}

void paint_native_button(const DRAWITEMSTRUCT &draw,
                         const NativeThemeColors &colors, const HFONT font,
                         const std::optional<COLORREF> explicit_fill) {
  const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
  const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
  const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;
  const COLORREF fill = disabled ? colors.fill
                        : pressed ? colors.selection
                        : hot     ? colors.hover
                                  : colors.fill;
  const HBRUSH background = CreateSolidBrush(fill);
  FillRect(draw.hDC, &draw.rcItem, background);
  DeleteObject(background);
  const HPEN pen =
      CreatePen(PS_SOLID, pressed ? 2 : 1,
                disabled ? colors.disabled
                : pressed ? colors.selection
                          : colors.border);
  const HGDIOBJ previous_pen = SelectObject(draw.hDC, pen);
  const HGDIOBJ previous_brush =
      SelectObject(draw.hDC, GetStockObject(NULL_BRUSH));
  Rectangle(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right,
            draw.rcItem.bottom);
  SelectObject(draw.hDC, previous_brush);
  SelectObject(draw.hDC, previous_pen);
  DeleteObject(pen);

  RECT text_rect = draw.rcItem;
  UINT text_format = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
  if (explicit_fill.has_value()) {
    const int item_height =
        static_cast<int>(draw.rcItem.bottom - draw.rcItem.top);
    const int swatch_size = std::min(14, std::max(8, item_height - 10));
    const int swatch_top =
        draw.rcItem.top + (draw.rcItem.bottom - draw.rcItem.top - swatch_size) / 2;
    RECT swatch{draw.rcItem.left + 7, swatch_top,
                draw.rcItem.left + 7 + swatch_size, swatch_top + swatch_size};
    const HBRUSH swatch_fill = CreateSolidBrush(*explicit_fill);
    FillRect(draw.hDC, &swatch, swatch_fill);
    DeleteObject(swatch_fill);
    const HBRUSH swatch_border =
        CreateSolidBrush(disabled ? colors.disabled : colors.border);
    FrameRect(draw.hDC, &swatch, swatch_border);
    DeleteObject(swatch_border);
    text_rect.left = swatch.right + 7;
    text_rect.right -= 5;
    text_format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
  }

  std::array<wchar_t, 160> label{};
  GetWindowTextW(draw.hwndItem, label.data(), static_cast<int>(label.size()));
  SetBkMode(draw.hDC, TRANSPARENT);
  SetTextColor(draw.hDC, disabled ? colors.disabled : colors.text);
  const HGDIOBJ previous_font = SelectObject(draw.hDC, font);
  DrawTextW(draw.hDC, label.data(), -1, &text_rect, text_format);
  SelectObject(draw.hDC, previous_font);
  if ((draw.itemState & ODS_FOCUS) != 0) {
    RECT focus = draw.rcItem;
    InflateRect(&focus, -3, -3);
    DrawFocusRect(draw.hDC, &focus);
  }
}

} // namespace layerc::platform
