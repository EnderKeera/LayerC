#include "layerc/platform/native_theme.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <dwmapi.h>
#include <uxtheme.h>

namespace layerc::platform {
namespace {

constexpr DWORD dark_title_bar_attribute = 20;
constexpr DWORD border_color_attribute = 34;
constexpr DWORD caption_color_attribute = 35;
constexpr DWORD caption_text_color_attribute = 36;

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

std::uint32_t system_accent() {
  DWORD colorization{};
  BOOL opaque{};
  if (SUCCEEDED(DwmGetColorizationColor(&colorization, &opaque)))
    return colorization & 0xFFFFFFU;
  return native_rgb_value(GetSysColor(COLOR_HIGHLIGHT));
}

// Relative luminance above ~0.4 reads better with dark content on top.
bool light_color(const COLORREF color) {
  const int luma = GetRValue(color) * 299 + GetGValue(color) * 587 +
                   GetBValue(color) * 114;
  return luma >= 140000;
}

NativeThemeColors default_theme_colors(const EditorTheme theme,
                                       const std::uint32_t accent,
                                       const EditorControlRole role) {
  const auto &base = neutral_theme;
  const COLORREF accent_color =
      native_color_ref(accent == 0 ? theme_accent(theme) : accent);
  const COLORREF window = native_color_ref(base.background);
  NativeThemeColors colors{
      .fill = window,
      .text = native_color_ref(base.text),
      .border = native_color_ref(base.border),
      .hover = native_color_ref(base.row_hover),
      // About a quarter of the accent over the window: a quiet selection
      // that keeps primary text above 7:1, marked by an accent rail.
      .selection = blend_color(window, accent_color, 60),
      .disabled = native_color_ref(base.disabled),
      .accent = accent_color,
      .on_accent = light_color(accent_color) ? RGB(0x14, 0x14, 0x14)
                                             : RGB(0xFF, 0xFF, 0xFF),
      .pressed = native_color_ref(base.pressed),
      .divider = native_color_ref(base.divider),
      .secondary = native_color_ref(base.secondary),
      .input = native_color_ref(base.input),
  };
  switch (role) {
  case EditorControlRole::toolbar:
    // Menu bar and status strip.
    colors.fill = native_color_ref(base.background);
    colors.hover = native_color_ref(base.button_hover);
    break;
  case EditorControlRole::sidebar:
    // The saved-profiles list.
    colors.fill = native_color_ref(base.input);
    break;
  case EditorControlRole::label:
  case EditorControlRole::status:
    colors.text = native_color_ref(base.secondary);
    break;
  case EditorControlRole::button:
    colors.fill = native_color_ref(base.panel);
    colors.hover = native_color_ref(base.button_hover);
    break;
  case EditorControlRole::checkbox:
    colors.hover = native_color_ref(base.button_hover);
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

std::atomic<int> text_scale_percent{100};

bool font_face_installed(const wchar_t *face) {
  LOGFONTW query{};
  query.lfCharSet = DEFAULT_CHARSET;
  wcsncpy_s(query.lfFaceName, face, _TRUNCATE);
  bool found = false;
  const HDC dc = GetDC(nullptr);
  if (dc == nullptr)
    return false;
  EnumFontFamiliesExW(
      dc, &query,
      [](const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM found_flag) {
        *reinterpret_cast<bool *>(found_flag) = true;
        return 0;
      },
      reinterpret_cast<LPARAM>(&found), 0);
  ReleaseDC(nullptr, dc);
  return found;
}

const wchar_t *ui_face() {
  static const wchar_t *const face =
      font_face_installed(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text"
                                                     : L"Segoe UI";
  return face;
}

const wchar_t *monospace_face() {
  static const wchar_t *const face =
      font_face_installed(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
  return face;
}

// Windows "Text size" enlarges the message font; keep its ratio to the
// 12 px (9 pt) default so the editor follows the accessibility setting.
int system_text_percent(const UINT dpi) {
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, metrics.cbSize,
                                 &metrics, 0, dpi) == FALSE ||
      metrics.lfMessageFont.lfHeight == 0)
    return 100;
  const int pixels = std::abs(metrics.lfMessageFont.lfHeight);
  return std::clamp(MulDiv(pixels, 100, MulDiv(12, static_cast<int>(dpi), 96)),
                    100, 225);
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

COLORREF blend_color(const COLORREF base, const COLORREF over,
                     const int amount) {
  const int alpha = std::clamp(amount, 0, 255);
  const auto channel = [alpha](const int from, const int to) {
    return static_cast<BYTE>(from + (to - from) * alpha / 255);
  };
  return RGB(channel(GetRValue(base), GetRValue(over)),
             channel(GetGValue(base), GetGValue(over)),
             channel(GetBValue(base), GetBValue(over)));
}

std::uint32_t theme_accent(const EditorTheme theme) {
  switch (theme) {
  case EditorTheme::system:
    return system_accent();
  case EditorTheme::dark:
    return slate_accent;
  case EditorTheme::teal:
    return teal_accent;
  case EditorTheme::plum:
    return plum_accent;
  case EditorTheme::copper:
  default:
    return copper_accent;
  }
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

NativeThemeColors
native_theme_colors(const EditorAppearanceSettings &appearance,
                    const EditorControlRole role) {
  return native_theme_colors(
      appearance.theme, appearance.accent, role,
      appearance.overrides[static_cast<std::size_t>(role)]);
}

void set_native_text_scale(const int percent) noexcept {
  text_scale_percent.store(std::clamp(percent, 100, 200));
}

int native_text_scale() noexcept { return text_scale_percent.load(); }

int native_text_percent(const UINT dpi) {
  return MulDiv(system_text_percent(dpi == 0 ? 96U : dpi), native_text_scale(),
                100);
}

HFONT create_native_font(const NativeFontKind kind, const UINT dpi) {
  const UINT effective_dpi = dpi == 0 ? 96U : dpi;
  const int scale = native_text_percent(effective_dpi);
  const int logical_pixels = kind == NativeFontKind::title ? 20 : 14;
  int height = MulDiv(MulDiv(logical_pixels, static_cast<int>(effective_dpi),
                             96),
                      scale, 100);
  if (kind == NativeFontKind::monospace)
    height = std::max(1, height - MulDiv(1, static_cast<int>(effective_dpi), 96));
  const int weight = kind == NativeFontKind::heading ||
                             kind == NativeFontKind::title
                         ? FW_SEMIBOLD
                         : FW_NORMAL;
  const wchar_t *face =
      kind == NativeFontKind::monospace ? monospace_face() : ui_face();
  return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY,
                     (kind == NativeFontKind::monospace ? FIXED_PITCH
                                                        : DEFAULT_PITCH) |
                         FF_DONTCARE,
                     face);
}

HFONT create_native_dialog_font(const UINT dpi) {
  return create_native_font(NativeFontKind::body, dpi);
}

HFONT create_native_monospace_font(const UINT dpi) {
  return create_native_font(NativeFontKind::monospace, dpi);
}

void fill_solid_rect(const HDC dc, const RECT &rect, const COLORREF color) {
  const HBRUSH brush = CreateSolidBrush(color);
  FillRect(dc, &rect, brush);
  DeleteObject(brush);
}

void frame_solid_rect(const HDC dc, const RECT &rect, const COLORREF color) {
  const HBRUSH brush = CreateSolidBrush(color);
  FrameRect(dc, &rect, brush);
  DeleteObject(brush);
}

void outline_rect(const HDC dc, const RECT &rect, const int pen_width,
                  const COLORREF color) {
  const HPEN pen = CreatePen(PS_SOLID | PS_INSIDEFRAME, pen_width, color);
  const HGDIOBJ previous_pen = SelectObject(dc, pen);
  const HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
  SelectObject(dc, previous_brush);
  SelectObject(dc, previous_pen);
  DeleteObject(pen);
}

void paint_focus_ring(const HDC dc, const RECT &rect, const COLORREF accent,
                      const UINT dpi) {
  const int width = std::max(2, MulDiv(2, dpi == 0 ? 96 : static_cast<int>(dpi),
                                       96));
  outline_rect(dc, rect, width, accent);
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

void set_window_app_icon(const HWND window) {
  const UINT dpi = GetDpiForWindow(window);
  const auto load = [dpi](const int metric) {
    const int size = GetSystemMetricsForDpi(metric, dpi == 0 ? 96U : dpi);
    return reinterpret_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr),
                   MAKEINTRESOURCEW(app_icon_resource), IMAGE_ICON, size,
                   size, LR_DEFAULTCOLOR | LR_SHARED));
  };
  if (const HICON big = load(SM_CXICON); big != nullptr)
    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
  if (const HICON small_icon = load(SM_CXSMICON); small_icon != nullptr)
    SendMessageW(window, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(small_icon));
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

void set_window_frame_colors(const HWND window, const COLORREF caption,
                             const COLORREF border, const COLORREF text) {
  (void)DwmSetWindowAttribute(window, caption_color_attribute, &caption,
                              sizeof(caption));
  (void)DwmSetWindowAttribute(window, border_color_attribute, &border,
                              sizeof(border));
  (void)DwmSetWindowAttribute(window, caption_text_color_attribute, &text,
                              sizeof(text));
}

} // namespace layerc::platform
