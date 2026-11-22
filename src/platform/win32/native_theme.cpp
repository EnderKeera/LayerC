#include "layerc/platform/native_theme.hpp"

#include "editor_color.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <dwmapi.h>
#include <uxtheme.h>

namespace layerc::platform {
using layerc::editor::rgb_accent_2;
using layerc::editor::rgb_contrast_ratio;
using layerc::editor::rgb_on_accent;
namespace {

constexpr DWORD dark_title_bar_attribute = 20;
constexpr DWORD border_color_attribute = 34;
constexpr DWORD caption_color_attribute = 35;
constexpr DWORD caption_text_color_attribute = 36;
constexpr DWORD window_corner_preference_attribute = 33;
constexpr int square_corners = 1; // DWMWCP_DONOTROUND

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

// -1 follows SPI_GETHIGHCONTRAST; 0 or 1 is the test seam's forced value.
std::atomic<int> high_contrast_override{-1};

} // namespace

// A Windows contrast theme: every role in the theme's system colors, with
// the preset and the Appearance overrides set aside.
NativeThemeColors high_contrast_colors(const EditorControlRole role) {
  const bool button = role == EditorControlRole::button;
  const COLORREF fill = GetSysColor(button ? COLOR_BTNFACE : COLOR_WINDOW);
  const COLORREF ink = GetSysColor(button ? COLOR_BTNTEXT : COLOR_WINDOWTEXT);
  const COLORREF gray = GetSysColor(COLOR_GRAYTEXT);
  const COLORREF highlight = GetSysColor(COLOR_HIGHLIGHT);
  return {
      .fill = fill,
      .text = ink,
      .border = ink,
      .hover = fill,
      .selection = highlight,
      .disabled = gray,
      .accent = highlight,
      .on_accent = GetSysColor(COLOR_HIGHLIGHTTEXT),
      .pressed = fill,
      .divider = gray,
      .secondary = ink,
      .input = GetSysColor(COLOR_WINDOW),
      .subtext = ink,
      .verbose = ink,
      .yes = ink,
      .no = ink,
      .warning = ink,
      .accent_2 = ink,
      .on_status = fill,
      .high_contrast = true,
  };
}

namespace {

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
      .on_accent = native_color_ref(rgb_on_accent(native_rgb_value(accent_color))),
      .pressed = native_color_ref(base.pressed),
      .divider = native_color_ref(base.divider),
      .secondary = native_color_ref(base.secondary),
      .input = native_color_ref(base.input),
      .subtext = native_color_ref(base.subtext),
      .verbose = native_color_ref(base.verbose),
      .yes = native_color_ref(base.yes),
      .no = native_color_ref(base.no),
      .warning = native_color_ref(base.warning),
      .accent_2 =
          native_color_ref(rgb_accent_2(native_rgb_value(accent_color))),
      .on_status = native_color_ref(base.on_status),
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

// The system message font's face (Segoe UI on Windows 10/11): the face every
// native dialog, message box and property sheet uses.
const wchar_t *ui_face() {
  static const std::array<wchar_t, LF_FACESIZE> face = [] {
    std::array<wchar_t, LF_FACESIZE> name{};
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize,
                              &metrics, 0) != FALSE &&
        metrics.lfMessageFont.lfFaceName[0] != L'\0' &&
        font_face_installed(metrics.lfMessageFont.lfFaceName))
      wcsncpy_s(name.data(), name.size(), metrics.lfMessageFont.lfFaceName,
                _TRUNCATE);
    else
      wcsncpy_s(name.data(), name.size(), L"Segoe UI", _TRUNCATE);
    return name;
  }();
  return face.data();
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

double contrast_ratio(const COLORREF first, const COLORREF second) {
  return rgb_contrast_ratio(native_rgb_value(first), native_rgb_value(second));
}

bool native_high_contrast() {
  if (const int forced = high_contrast_override.load(); forced >= 0)
    return forced != 0;
  HIGHCONTRASTW contrast{};
  contrast.cbSize = sizeof(contrast);
  return SystemParametersInfoW(SPI_GETHIGHCONTRAST, contrast.cbSize,
                               &contrast, 0) != FALSE &&
         (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

void set_native_high_contrast_override(
    const std::optional<bool> enabled) noexcept {
  high_contrast_override.store(enabled ? (*enabled ? 1 : 0) : -1);
}

NativeThemeColors native_theme_colors(
    const EditorTheme theme, const std::uint32_t accent,
    const EditorControlRole role,
    const EditorAppearanceOverrides &overrides) {
  if (native_high_contrast())
    return high_contrast_colors(role);
  return preset_theme_colors(theme, accent, role, overrides);
}

NativeThemeColors preset_theme_colors(
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
  const int logical_pixels = kind == NativeFontKind::title ? 16 : 12;
  const int height = MulDiv(
      MulDiv(logical_pixels, static_cast<int>(effective_dpi), 96), scale, 100);
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

COLORREF status_color(const NativeThemeColors &colors,
                      const StatusGlyph glyph) {
  switch (glyph) {
  case StatusGlyph::yes:
    return colors.yes;
  case StatusGlyph::no:
    return colors.no;
  case StatusGlyph::warning:
    return colors.warning;
  case StatusGlyph::accent_2:
  default:
    return colors.accent_2;
  }
}

int status_glyph_size(const UINT dpi) {
  const UINT effective_dpi = dpi == 0 ? 96U : dpi;
  return MulDiv(MulDiv(12, static_cast<int>(effective_dpi), 96),
                native_text_percent(effective_dpi), 100);
}

RECT paint_status_glyph(const HDC dc, const RECT &bounds,
                        const StatusGlyph glyph, const COLORREF color,
                        const UINT dpi) {
  const int height = std::max(1L, bounds.bottom - bounds.top);
  const int size = std::min(status_glyph_size(dpi), height);
  const int top = bounds.top + (height - size) / 2;
  const RECT box{bounds.left, top, bounds.left + size, top + size};
  // Positions on the glyphs' 12-unit grid, in pixels.
  const auto at = [size](const double grid) {
    return static_cast<int>(std::lround(grid * size / 12.0));
  };
  const auto square = [&](const double left, const double square_top,
                          const double width, const double square_height) {
    fill_solid_rect(dc,
                    {box.left + at(left), box.top + at(square_top),
                     box.left + at(left + width),
                     box.top + at(square_top + square_height)},
                    color);
  };
  switch (glyph) {
  case StatusGlyph::warning:
    square(5, 1, 2, 7);
    square(5, 9.5, 2, 2);
    break;
  case StatusGlyph::accent_2:
    square(1, 8, 2, 2);
    square(5, 8, 2, 2);
    square(9, 8, 2, 2);
    break;
  case StatusGlyph::yes:
  case StatusGlyph::no:
  default: {
    // 2-unit strokes with square caps, as in the Glyphs SVGs.
    const LOGBRUSH ink{.lbStyle = BS_SOLID, .lbColor = color, .lbHatch = 0};
    const HPEN pen = ExtCreatePen(PS_SOLID | PS_GEOMETRIC | PS_ENDCAP_SQUARE |
                                      PS_JOIN_MITER,
                                  static_cast<DWORD>(std::max(2, at(2))), &ink,
                                  0, nullptr);
    const HGDIOBJ previous = SelectObject(dc, pen);
    const auto point = [&](const double x, const double y) {
      return POINT{box.left + at(x), box.top + at(y)};
    };
    if (glyph == StatusGlyph::yes) {
      const std::array<POINT, 3> tick{point(2.5, 6.5), point(5, 9),
                                      point(9.5, 3.5)};
      Polyline(dc, tick.data(), static_cast<int>(tick.size()));
    } else {
      const std::array<POINT, 2> down{point(3, 3), point(9, 9)};
      const std::array<POINT, 2> up{point(9, 3), point(3, 9)};
      Polyline(dc, down.data(), static_cast<int>(down.size()));
      Polyline(dc, up.data(), static_cast<int>(up.size()));
    }
    SelectObject(dc, previous);
    DeleteObject(pen);
    break;
  }
  }
  return box;
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

UINT native_window_dpi(const HWND window) {
  const UINT dpi = GetDpiForWindow(window);
  return dpi == 0 ? 96U : dpi;
}

void set_window_app_icon(const HWND window) {
  const UINT dpi = native_window_dpi(window);
  const auto load = [dpi](const int metric) {
    const int size = GetSystemMetricsForDpi(metric, dpi);
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
  // Square frame corners match the square controls (Windows 11; ignored on
  // Windows 10, whose corners are already square).
  (void)DwmSetWindowAttribute(window, window_corner_preference_attribute,
                              &square_corners, sizeof(square_corners));
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
