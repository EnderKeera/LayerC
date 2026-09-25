#pragma once

#include "layerc/platform/editor_appearance.hpp"

#include <cstdint>
#include <optional>

#include <windows.h>

namespace layerc::platform {

struct NativeThemeColors {
  COLORREF fill{};
  COLORREF text{};
  COLORREF border{};
  COLORREF hover{};
  COLORREF selection{};
  COLORREF disabled{};
};

// Base palette per editor theme; also the swatch source for the color picker.
struct ThemeBase {
  std::uint32_t background{};
  std::uint32_t panel{};
  std::uint32_t input{};
  std::uint32_t border{};
  std::uint32_t text{};
  std::uint32_t muted{};
  std::uint32_t accent{};
};

inline constexpr ThemeBase copper_theme{0x1B1F24, 0x282D34, 0x20252B, 0x4B525C,
                                        0xE9E4DD, 0x9CA3AD, 0xD88E4B};
inline constexpr ThemeBase teal_theme{0x182021, 0x263436, 0x1F292B, 0x466367,
                                      0xE1ECEA, 0x96AAA8, 0x5AB6AA};
inline constexpr ThemeBase plum_theme{0x211E29, 0x312B3C, 0x282231, 0x5A506B,
                                      0xECE7F1, 0xB0A7BC, 0xB382E2};
inline constexpr ThemeBase dark_theme{0x1B1F24, 0x282D34, 0x20252B, 0x4B525C,
                                      0xE9E4DD, 0x9CA3AD, 0x8294A6};

[[nodiscard]] COLORREF native_color_ref(std::uint32_t color);
[[nodiscard]] std::uint32_t native_rgb_value(COLORREF color);
[[nodiscard]] NativeThemeColors native_theme_colors(
    EditorTheme theme, std::uint32_t accent, EditorControlRole role,
    const EditorAppearanceOverrides &overrides);
// One role of a full appearance, including that role's overrides.
[[nodiscard]] NativeThemeColors
native_theme_colors(const EditorAppearanceSettings &appearance,
                    EditorControlRole role);
// Segoe UI 9 pt at `dpi` (0 means 96): the shared native dialog font.
[[nodiscard]] HFONT create_native_dialog_font(UINT dpi);
// Fill or frame a rectangle through a temporary solid brush, or outline it
// with a temporary pen, restoring the DC's selected objects.
void fill_solid_rect(HDC dc, const RECT &rect, COLORREF color);
void frame_solid_rect(HDC dc, const RECT &rect, COLORREF color);
void outline_rect(HDC dc, const RECT &rect, int pen_width, COLORREF color);

void enable_native_dark_mode();
void set_dark_title_bar(HWND window);
void style_native_dialog_children(HWND dialog, HFONT font,
                                  bool owner_draw_list_boxes = false);
void paint_native_button(const DRAWITEMSTRUCT &draw,
                         const NativeThemeColors &colors, HFONT font,
                         std::optional<COLORREF> explicit_fill = std::nullopt);

} // namespace layerc::platform
