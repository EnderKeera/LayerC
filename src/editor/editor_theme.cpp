#include "editor_window.hpp"
#include "editor_resource.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windows.h>
namespace layerc::editor {
constexpr int dark_title_bar_attribute = 20;

ProfileCheckboxMetrics profile_checkbox_metrics(const UINT source_dpi) {
  const int dpi = source_dpi == 0 ? 96 : static_cast<int>(source_dpi);
  return {
      .image_size = std::max(1, MulDiv(16, dpi, 96)),
      .inset = std::max(1, MulDiv(2, dpi, 96)),
      .pen_width = std::max(1, MulDiv(2, dpi, 96)),
  };
}

void set_owner_draw_checkbox(const HWND control, const bool checked) {
  if (control == nullptr) return;
  SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
  InvalidateRect(control, nullptr, TRUE);
}

[[nodiscard]] bool owner_draw_checkbox_checked(const HWND control) {
  return control != nullptr &&
         SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

struct ThemeBase {
  std::uint32_t background{};
  std::uint32_t panel{};
  std::uint32_t input{};
  std::uint32_t border{};
  std::uint32_t text{};
  std::uint32_t muted{};
  std::uint32_t accent{};
};

constexpr ThemeBase copper{0x1B1F24, 0x282D34, 0x20252B, 0x4B525C,
                           0xE9E4DD, 0x9CA3AD, 0xD88E4B};
constexpr ThemeBase teal{0x182021, 0x263436, 0x1F292B, 0x466367,
                         0xE1ECEA, 0x96AAA8, 0x5AB6AA};
constexpr ThemeBase plum{0x211E29, 0x312B3C, 0x282231, 0x5A506B,
                         0xECE7F1, 0xB0A7BC, 0xB382E2};

COLORREF color_ref(const std::uint32_t color) {
  return RGB((color >> 16U) & 0xFFU, (color >> 8U) & 0xFFU,
             color & 0xFFU);
}

std::uint32_t rgb_value(const COLORREF color) {
  return (static_cast<std::uint32_t>(GetRValue(color)) << 16U) |
         (static_cast<std::uint32_t>(GetGValue(color)) << 8U) |
         static_cast<std::uint32_t>(GetBValue(color));
}

const ThemeBase &theme_base(const layerc::platform::EditorTheme theme) {
  switch (theme) {
  case layerc::platform::EditorTheme::teal:
    return teal;
  case layerc::platform::EditorTheme::plum:
    return plum;
  case layerc::platform::EditorTheme::copper:
  default:
    return copper;
  }
}

RoleColors default_colors(const layerc::platform::EditorTheme theme,
                          const std::uint32_t accent,
                          const layerc::platform::EditorControlRole role) {
  const auto &base = theme_base(theme);
  const auto accent_color = accent == 0 ? base.accent : accent;
  const auto accent_dark =
      ((accent_color & 0xFF0000U) * 55U / 100U & 0xFF0000U) |
      ((accent_color & 0x00FF00U) * 55U / 100U & 0x00FF00U) |
      ((accent_color & 0x0000FFU) * 55U / 100U & 0x0000FFU);
  RoleColors colors{.fill = color_ref(base.background),
                    .text = color_ref(base.text),
                    .border = color_ref(base.border),
                    .hover = color_ref(base.panel),
                    .selection = color_ref(accent_dark),
                    .disabled = color_ref(base.muted)};
  switch (role) {
  case layerc::platform::EditorControlRole::toolbar:
    colors.fill = color_ref(base.background);
    break;
  case layerc::platform::EditorControlRole::sidebar:
    colors.fill = color_ref(base.panel);
    break;
  case layerc::platform::EditorControlRole::label:
  case layerc::platform::EditorControlRole::status:
    colors.text = color_ref(base.muted);
    break;
  case layerc::platform::EditorControlRole::button:
  case layerc::platform::EditorControlRole::checkbox:
    colors.fill = color_ref(base.panel);
    colors.hover = color_ref(base.input);
    break;
  case layerc::platform::EditorControlRole::edit:
  case layerc::platform::EditorControlRole::list:
  case layerc::platform::EditorControlRole::preview:
    colors.fill = color_ref(base.input);
    break;
  case layerc::platform::EditorControlRole::window:
  case layerc::platform::EditorControlRole::count:
    break;
  }
  return colors;
}

RoleColors resolved_colors(const layerc::platform::EditorTheme theme,
                           const std::uint32_t accent,
                           const layerc::platform::EditorControlRole role) {
  auto colors = default_colors(theme, accent, role);
  const auto overrides = layerc::platform::editor_appearance_overrides(role);
  if ((overrides.fields & layerc::platform::editor_fill) != 0) colors.fill = color_ref(overrides.fill);
  if ((overrides.fields & layerc::platform::editor_text) != 0) colors.text = color_ref(overrides.text);
  if ((overrides.fields & layerc::platform::editor_border) != 0) colors.border = color_ref(overrides.border);
  if ((overrides.fields & layerc::platform::editor_hover) != 0) colors.hover = color_ref(overrides.hover);
  if ((overrides.fields & layerc::platform::editor_selection) != 0) colors.selection = color_ref(overrides.selection);
  if ((overrides.fields & layerc::platform::editor_disabled) != 0) colors.disabled = color_ref(overrides.disabled);
  return colors;
}


enum class PreferredAppMode : int {
  default_mode,
  allow_dark,
  force_dark,
  force_light,
  maximum,
};

HMODULE uxtheme_module() {
  static const HMODULE module = [] {
    HMODULE loaded = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return loaded != nullptr ? loaded : LoadLibraryW(L"uxtheme.dll");
  }();
  return module;
}

void enable_native_dark_mode() {
  const HMODULE module = uxtheme_module();
  if (module == nullptr) return;
  using SetPreferredAppMode = PreferredAppMode(WINAPI *)(PreferredAppMode);
  using FlushMenuThemes = void(WINAPI *)();
  if (const auto set_mode = reinterpret_cast<SetPreferredAppMode>(
          GetProcAddress(module, MAKEINTRESOURCEA(135)));
      set_mode != nullptr)
    (void)set_mode(PreferredAppMode::allow_dark);
  if (const auto flush = reinterpret_cast<FlushMenuThemes>(
          GetProcAddress(module, MAKEINTRESOURCEA(136)));
      flush != nullptr)
    flush();
}

void set_dark_title_bar(const HWND window) {
  if (const HMODULE module = uxtheme_module(); module != nullptr) {
    using AllowDarkModeForWindow = BOOL(WINAPI *)(HWND, BOOL);
    if (const auto allow = reinterpret_cast<AllowDarkModeForWindow>(
            GetProcAddress(module, MAKEINTRESOURCEA(133)));
        allow != nullptr)
      (void)allow(window, TRUE);
  }
  SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
  const BOOL enabled = TRUE;
  (void)DwmSetWindowAttribute(window, dark_title_bar_attribute, &enabled, sizeof(enabled));
}


void style_native_dialog_children(const HWND dialog, const HFONT font) {
  for (HWND child = GetWindow(dialog, GW_CHILD); child != nullptr;
       child = GetWindow(child, GW_HWNDNEXT)) {
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    std::array<wchar_t, 64> class_name{};
    GetClassNameW(child, class_name.data(), static_cast<int>(class_name.size()));
    if (wcscmp(class_name.data(), L"Button") == 0) {
      const auto style = GetWindowLongPtrW(child, GWL_STYLE);
      const auto button_type = style & static_cast<LONG_PTR>(BS_TYPEMASK);
      if (button_type == BS_PUSHBUTTON) {
        SetWindowLongPtrW(
            child, GWL_STYLE,
            (style & ~static_cast<LONG_PTR>(BS_TYPEMASK)) | BS_OWNERDRAW);
        SetWindowPos(child, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
      } else {
        // Keep BS_DEFPUSHBUTTON and the native auto-check/radio styles.  The
        // dialog manager relies on those type bits for Enter, Space, and
        // accessibility semantics.
        SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
      }
    } else if (wcscmp(class_name.data(), L"ComboBox") == 0) {
      SetWindowTheme(child, L"DarkMode_CFD", nullptr);
    } else if (wcscmp(class_name.data(), L"ListBox") == 0) {
      const auto style = GetWindowLongPtrW(child, GWL_STYLE);
      SetWindowLongPtrW(child, GWL_STYLE,
                        style | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS);
      const UINT dpi = GetDpiForWindow(dialog);
      SendMessageW(child, LB_SETITEMHEIGHT, 0,
                   static_cast<LPARAM>(MulDiv(18, dpi == 0 ? 96 : static_cast<int>(dpi), 96)));
      SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
    } else if (wcscmp(class_name.data(), L"Edit") == 0 ||
               wcscmp(class_name.data(), WC_LISTVIEWW) == 0) {
      SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
    }
  }
}

void EditorWindow::style_dialog_children(const HWND dialog) const {
  style_native_dialog_children(dialog, font_);
}

void EditorWindow::style_list_view(const HWND list, const bool hide_header) const {
  if (list == nullptr) return;
  auto style = GetWindowLongPtrW(list, GWL_STYLE);
  if (hide_header) style |= LVS_NOCOLUMNHEADER;
  SetWindowLongPtrW(list, GWL_STYLE, style);
  const auto list_colors = colors(layerc::platform::EditorControlRole::list);
  ListView_SetBkColor(list, list_colors.fill);
  ListView_SetTextBkColor(list, list_colors.fill);
  ListView_SetTextColor(list, list_colors.text);
  SetWindowTheme(list, L"DarkMode_Explorer", nullptr);
  if (const HWND header = ListView_GetHeader(list); header != nullptr)
    SetWindowTheme(header, L"DarkMode_ItemsView", nullptr);
  SetWindowPos(list, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void EditorWindow::rebuild_profile_check_images() {
  if (profile_list_ == nullptr) return;
  const auto metrics = profile_checkbox_metrics(GetDpiForWindow(profile_list_));
  const HIMAGELIST images = ImageList_Create(
      metrics.image_size, metrics.image_size, ILC_COLOR24, 2, 0);
  const HDC screen = GetDC(profile_list_);
  const HDC memory = screen == nullptr ? nullptr : CreateCompatibleDC(screen);
  if (images == nullptr || screen == nullptr || memory == nullptr) {
    if (memory != nullptr) DeleteDC(memory);
    if (screen != nullptr) ReleaseDC(profile_list_, screen);
    if (images != nullptr) ImageList_Destroy(images);
    return;
  }

  const auto list_colors = colors(layerc::platform::EditorControlRole::list);
  const auto check_colors = colors(layerc::platform::EditorControlRole::checkbox);
  for (const bool checked : {false, true}) {
    const HBITMAP bitmap = CreateCompatibleBitmap(
        screen, metrics.image_size, metrics.image_size);
    const HGDIOBJ previous_bitmap = SelectObject(memory, bitmap);
    RECT image_rect{0, 0, metrics.image_size, metrics.image_size};
    const HBRUSH background = CreateSolidBrush(list_colors.fill);
    FillRect(memory, &image_rect, background);
    DeleteObject(background);

    RECT box{metrics.inset, metrics.inset,
             metrics.image_size - metrics.inset,
             metrics.image_size - metrics.inset};
    const HBRUSH box_fill = CreateSolidBrush(checked ? check_colors.selection
                                                     : check_colors.hover);
    FillRect(memory, &box, box_fill);
    DeleteObject(box_fill);
    const HBRUSH border = CreateSolidBrush(check_colors.border);
    FrameRect(memory, &box, border);
    DeleteObject(border);
    if (checked) {
      const HPEN tick =
          CreatePen(PS_SOLID, metrics.pen_width, check_colors.text);
      const HGDIOBJ previous_pen = SelectObject(memory, tick);
      const int box_size = box.right - box.left;
      MoveToEx(memory, box.left + box_size / 6,
               box.top + box_size / 2, nullptr);
      LineTo(memory, box.left + box_size * 5 / 12,
             box.bottom - box_size / 4);
      LineTo(memory, box.right - box_size / 6,
             box.top + box_size / 4);
      SelectObject(memory, previous_pen);
      DeleteObject(tick);
    }
    SelectObject(memory, previous_bitmap);
    ImageList_Add(images, bitmap, nullptr);
    DeleteObject(bitmap);
  }
  DeleteDC(memory);
  ReleaseDC(profile_list_, screen);
  ImageList_SetBkColor(images, list_colors.fill);
  (void)ListView_SetImageList(profile_list_, images, LVSIL_STATE);
  if (profile_check_images_ != nullptr) ImageList_Destroy(profile_check_images_);
  profile_check_images_ = images;
}
void EditorWindow::rebuild_brushes() {
  for (auto &brush_value : brushes_) {
    if (brush_value != nullptr) DeleteObject(brush_value);
    brush_value = nullptr;
  }
  for (std::uint32_t index = 0;
       index < static_cast<std::uint32_t>(layerc::platform::EditorControlRole::count); ++index) {
    const auto role = static_cast<layerc::platform::EditorControlRole>(index);
    brushes_[index] = CreateSolidBrush(colors(role).fill);
  }
}

void EditorWindow::refresh_theme() {
  rebuild_brushes();
  style_list_view(profile_list_, true);
  rebuild_profile_check_images();
  style_list_view(task_list_, false);
  if (dialog_ != nullptr) {
    DrawMenuBar(dialog_);
    InvalidateRect(dialog_, nullptr, TRUE);
    EnumChildWindows(dialog_, [](HWND child, LPARAM) -> BOOL {
      InvalidateRect(child, nullptr, TRUE); return TRUE;
    }, 0);
  }
  if (task_dialog_ != nullptr) InvalidateRect(task_dialog_, nullptr, TRUE);
}


} // namespace layerc::editor
