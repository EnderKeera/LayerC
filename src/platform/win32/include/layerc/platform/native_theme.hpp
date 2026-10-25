#pragma once

#include "layerc/platform/editor_appearance.hpp"

#include <cstdint>
#include <optional>

#include <windows.h>

#include <commctrl.h>

namespace layerc::platform {

// Role colors. The first six are the user-overridable Appearance fields; the
// rest derive from the preset and are shared by every role.
struct NativeThemeColors {
  COLORREF fill{};
  COLORREF text{};
  COLORREF border{};
  COLORREF hover{};
  COLORREF selection{};
  COLORREF disabled{};
  COLORREF accent{};
  COLORREF on_accent{};
  COLORREF pressed{};
  COLORREF divider{};
  COLORREF secondary{};
  COLORREF input{};
};

// Neutral dark ramp shared by every preset (Windows 11 / Fluent dark values);
// presets differ only in their accent. Also the color picker's swatch source.
struct ThemeBase {
  std::uint32_t background{};
  std::uint32_t panel{};
  std::uint32_t input{};
  std::uint32_t border{};
  std::uint32_t divider{};
  std::uint32_t text{};
  std::uint32_t secondary{};
  std::uint32_t disabled{};
  std::uint32_t button_hover{};
  std::uint32_t row_hover{};
  std::uint32_t pressed{};
  std::uint32_t accent{};
};

inline constexpr ThemeBase neutral_theme{
    .background = 0x1F1F1F,
    .panel = 0x2D2D2D,
    .input = 0x171717,
    .border = 0x6E6E6E,
    .divider = 0x333333,
    .text = 0xF3F3F3,
    .secondary = 0xB8B8B8,
    .disabled = 0x808080,
    .button_hover = 0x383838,
    .row_hover = 0x2A2A2A,
    .pressed = 0x262626,
    .accent = 0xD88E4B,
};
inline constexpr std::uint32_t copper_accent = 0xD88E4B;
inline constexpr std::uint32_t teal_accent = 0x5AB6AA;
inline constexpr std::uint32_t plum_accent = 0xB382E2;
inline constexpr std::uint32_t slate_accent = 0x8FA3B8;

[[nodiscard]] COLORREF native_color_ref(std::uint32_t color);
[[nodiscard]] std::uint32_t native_rgb_value(COLORREF color);
// `amount` of `over` (0-255) composited onto `base`.
[[nodiscard]] COLORREF blend_color(COLORREF base, COLORREF over, int amount);
// Preset accent; `system` follows the Windows accent color.
[[nodiscard]] std::uint32_t theme_accent(EditorTheme theme);
[[nodiscard]] NativeThemeColors native_theme_colors(
    EditorTheme theme, std::uint32_t accent, EditorControlRole role,
    const EditorAppearanceOverrides &overrides);
// One role of a full appearance, including that role's overrides.
[[nodiscard]] NativeThemeColors
native_theme_colors(const EditorAppearanceSettings &appearance,
                    EditorControlRole role);

// Text size in percent (100, 115 or 130) applied to every native font and to
// DPI-scaled dialog geometry. Values are clamped to [100, 200].
void set_native_text_scale(int percent) noexcept;
[[nodiscard]] int native_text_scale() noexcept;
// Windows text size (relative to the 12 px message font) times
// native_text_scale(), in percent: how much larger than 96-DPI design units
// text renders at `dpi`. Geometry that must fit text scales by this too.
[[nodiscard]] int native_text_percent(UINT dpi);
// GetDpiForWindow, or the 96-DPI baseline when the window reports none.
[[nodiscard]] UINT native_window_dpi(HWND window);

enum class NativeFontKind { body, heading, title, monospace };
// Body text is 14 px at 96 DPI and 100 %, adjusted by the Windows text-size
// setting (the message font's size relative to its 12 px default) and by
// native_text_scale(). Face: Segoe UI Variable Text, else Segoe UI; headings
// are the same face semibold, titles semibold 20 px, monospace Cascadia Mono,
// else Consolas.
[[nodiscard]] HFONT create_native_font(NativeFontKind kind, UINT dpi);
// The shared native dialog (body) font at `dpi` (0 means 96).
[[nodiscard]] HFONT create_native_dialog_font(UINT dpi);
[[nodiscard]] HFONT create_native_monospace_font(UINT dpi);

// Fill or frame a rectangle through a temporary solid brush, or outline it
// with a temporary pen, restoring the DC's selected objects.
void fill_solid_rect(HDC dc, const RECT &rect, COLORREF color);
void frame_solid_rect(HDC dc, const RECT &rect, COLORREF color);
void outline_rect(HDC dc, const RECT &rect, int pen_width, COLORREF color);
// The single keyboard-focus indicator: a 2 px accent ring (scaled by DPI).
void paint_focus_ring(HDC dc, const RECT &rect, COLORREF accent, UINT dpi);

void enable_native_dark_mode();
void set_dark_title_bar(HWND window);
// Resource id of every LayerC program's application icon (first ICON, so
// Explorer shows it too).
inline constexpr int app_icon_resource = 1;
// Sets the executable's app icon on a dialog-based top-level window (dialogs
// have no class icon), sized for the window's DPI.
void set_window_app_icon(HWND window);
// Windows 11 caption, border and caption-text colors; ignored elsewhere.
void set_window_frame_colors(HWND window, COLORREF caption, COLORREF border,
                             COLORREF text);

// Colors read by the control subclasses installed below, which paint outside
// WM_DRAWITEM/WM_CTLCOLOR* and so cannot ask their dialog. `frame` is the
// edit role (border, input fill, accent); `menu` the toolbar role (menu bar).
struct NativeChromePalette {
  NativeThemeColors frame;
  NativeThemeColors menu;
  // Button role, for surfaces painted without their owner's help (prompts).
  NativeThemeColors button;
};
void set_native_chrome_palette(const NativeChromePalette &palette);
[[nodiscard]] NativeChromePalette native_chrome_palette();

// Converts push buttons to owner draw (with hover tracking), gives edits,
// list boxes and list views a flat 1 px frame with an accent focus edge,
// and applies dark themes. Checkboxes and radio buttons keep their native
// type and are painted through NM_CUSTOMDRAW (native_button_custom_draw).
void style_native_dialog_children(HWND dialog, HFONT font,
                                  bool owner_draw_list_boxes = false);
// Pointer-hover tracking for owner-drawn buttons: `native_button_hot` is true
// between the first WM_MOUSEMOVE and WM_MOUSELEAVE.
void track_native_button_hover(HWND button);
[[nodiscard]] bool native_button_hot(HWND button);
// Hovered row of a list box or list view (-1 when none).
void track_native_list_hover(HWND list);
[[nodiscard]] int native_list_hot_item(HWND list);
// Flat 1 px border drawn over the classic client edge; 2 px accent bottom
// edge while focused.
void install_flat_frame(HWND control);
// Paints a list view's column headers in the list's own text color, which
// the dark header theme does not apply on its own.
void install_list_header_text(HWND list);
// Dark menu bar (WM_UAHDRAWMENU / WM_UAHDRAWMENUITEM) and the 1 px line
// Windows draws under it.
void install_dark_menu_bar(HWND window);

struct NativeCheckState {
  bool checked{};
  bool disabled{};
  bool hot{};
  bool pressed{};
  bool radio{};
};
// Draws the check box glyph vertically centered at the left of `bounds` and
// returns its rectangle. `dpi` sizes the 18 px (at 96 DPI) glyph.
RECT paint_native_check_box(HDC dc, const RECT &bounds,
                            const NativeCheckState &state,
                            const NativeThemeColors &colors, UINT dpi);
[[nodiscard]] int native_check_box_size(UINT dpi);
// NM_CUSTOMDRAW for checkbox/radio buttons (Common Controls 6): paints the
// whole control and returns CDRF_SKIPDEFAULT, or nullopt for other buttons.
[[nodiscard]] std::optional<LRESULT>
native_button_custom_draw(const NMHDR &header, const NativeThemeColors &colors,
                          HFONT font);
void paint_native_button(const DRAWITEMSTRUCT &draw,
                         const NativeThemeColors &colors, HFONT font,
                         std::optional<COLORREF> explicit_fill = std::nullopt);

} // namespace layerc::platform
