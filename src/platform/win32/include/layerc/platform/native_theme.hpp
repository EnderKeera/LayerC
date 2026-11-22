#pragma once

#include "layerc/platform/editor_appearance.hpp"

#include <array>
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
  // Hints beside a field, and mono diagnostics (below `secondary`).
  COLORREF subtext{};
  COLORREF verbose{};
  // Status text and glyphs (never a control fill), the secondary color for
  // neutral information, and the text on a status fill.
  COLORREF yes{};
  COLORREF no{};
  COLORREF warning{};
  COLORREF accent_2{};
  COLORREF on_status{};
  // A Windows contrast theme supplied these colors (native_high_contrast):
  // painters then keep hover and pressed fills, outline them in `accent`,
  // and put `on_accent` on every `selection` and `accent` fill.
  bool high_contrast{};
};

[[nodiscard]] NativeThemeColors high_contrast_colors(EditorControlRole role);

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
  std::uint32_t subtext{};
  std::uint32_t verbose{};
  std::uint32_t yes{};
  std::uint32_t no{};
  std::uint32_t warning{};
  std::uint32_t on_status{};
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
    .subtext = 0xA3A3A3,
    .verbose = 0x9A9A9A,
    .yes = 0x00E676,
    .no = 0xFF5A5A,
    .warning = 0xFFD740,
    .on_status = 0x141414,
};
inline constexpr std::uint32_t copper_accent = 0xD88E4B;
inline constexpr std::uint32_t teal_accent = 0x5AB6AA;
inline constexpr std::uint32_t plum_accent = 0xB382E2;
inline constexpr std::uint32_t slate_accent = 0x8FA3B8;

// Choose color's palette; order and names are preserved.
struct PaletteColor {
  std::uint32_t value;
  std::wstring_view name;
};
inline constexpr std::array<PaletteColor, 24> color_palette{
    PaletteColor{neutral_theme.background, L"Graphite"},
    PaletteColor{neutral_theme.panel, L"Charcoal"},
    PaletteColor{neutral_theme.border, L"Gray"},
    PaletteColor{neutral_theme.secondary, L"Silver"},
    PaletteColor{0xE9E4DD, L"Warm white"},
    PaletteColor{0xF1F5F7, L"Ice white"},
    PaletteColor{copper_accent, L"Copper"},
    PaletteColor{0xE0A15A, L"Amber"},
    PaletteColor{0xE85D5D, L"Red"},
    PaletteColor{0xF08A5D, L"Coral"},
    PaletteColor{0xE7C84B, L"Gold"},
    PaletteColor{0x8BC34A, L"Lime"},
    PaletteColor{0x4FCB83, L"Green"},
    PaletteColor{teal_accent, L"Seafoam"},
    PaletteColor{0x50BAC8, L"Teal"},
    PaletteColor{0x5DADE2, L"Sky blue"},
    PaletteColor{0x4B7BEC, L"Blue"},
    PaletteColor{0x6C5CE7, L"Indigo"},
    PaletteColor{0x9B72E8, L"Violet"},
    PaletteColor{plum_accent, L"Plum"},
    PaletteColor{0xD65DB1, L"Magenta"},
    PaletteColor{0xF06292, L"Pink"},
    PaletteColor{0x8D6E63, L"Brown"},
    PaletteColor{0x607D8B, L"Blue gray"},
};

[[nodiscard]] COLORREF native_color_ref(std::uint32_t color);
[[nodiscard]] std::uint32_t native_rgb_value(COLORREF color);
// `amount` of `over` (0-255) composited onto `base`.
[[nodiscard]] COLORREF blend_color(COLORREF base, COLORREF over, int amount);
// Preset accent; `system` follows the Windows accent color.
[[nodiscard]] std::uint32_t theme_accent(EditorTheme theme);
// WCAG 2 contrast ratio of two colors (1 to 21).
[[nodiscard]] double contrast_ratio(COLORREF first, COLORREF second);
// True while a Windows contrast theme is on (SPI_GETHIGHCONTRAST): every
// role then paints in the theme's system colors, setting the preset and its
// overrides aside.
[[nodiscard]] bool native_high_contrast();
// Test seam: forces native_high_contrast() on or off; nullopt follows the
// system setting again.
void set_native_high_contrast_override(std::optional<bool> enabled) noexcept;
[[nodiscard]] NativeThemeColors native_theme_colors(
    EditorTheme theme, std::uint32_t accent, EditorControlRole role,
    const EditorAppearanceOverrides &overrides);
// The preset and its overrides alone, ignoring a contrast theme: what
// Appearance validates and the contrast-floor tests measure.
[[nodiscard]] NativeThemeColors preset_theme_colors(
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
// Body text is the native 12 px (9 pt) at 96 DPI and 100 %, adjusted by the
// Windows text-size setting (the message font's size relative to its 12 px
// default) and by native_text_scale(). Face: the system message font (Segoe
// UI); headings are the same face semibold, titles semibold 16 px, monospace
// Cascadia Mono, else Consolas, at the body size.
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

// The drawn status glyphs (never font characters): yes ✓, no ✕, warning
// !, accent_2 … (neutral information). Each pairs with its status word.
enum class StatusGlyph { yes, no, warning, accent_2 };
[[nodiscard]] COLORREF status_color(const NativeThemeColors &colors,
                                    StatusGlyph glyph);
// Side of the square glyph: 12 px at 96 DPI and 100 %, scaled like body text.
[[nodiscard]] int status_glyph_size(UINT dpi);
// Paints `glyph` in `color`, vertically centered at the left of `bounds`, on
// the 12 px grid of the design system's Glyphs SVGs (2 px square strokes);
// returns the glyph's square.
RECT paint_status_glyph(HDC dc, const RECT &bounds, StatusGlyph glyph,
                        COLORREF color, UINT dpi);

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
// list boxes, list views and bordered statics a flat 1 px frame
// (install_flat_frame), and applies dark themes. Checkboxes and radio buttons keep their native
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
// Flat 1 px border drawn over the classic client edge. While focused, an
// edit's border becomes the 2 px accent ring; a list keeps its border and
// takes a 2 px accent bottom edge (its focused row carries the ring).
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
// returns its rectangle. `dpi` sizes the 16 px (at 96 DPI) glyph.
RECT paint_native_check_box(HDC dc, const RECT &bounds,
                            const NativeCheckState &state,
                            const NativeThemeColors &colors, UINT dpi);
[[nodiscard]] int native_check_box_size(UINT dpi);
// Where a check box's label starts: the box, then 8 px (DesignSystem.md,
// CheckBox). Layouts indent a check box's hint by the same amount.
[[nodiscard]] int native_check_box_label_offset(UINT dpi);
// The room the focus ring takes after a check box's label text.
[[nodiscard]] int native_check_box_ring_margin(UINT dpi);
// NM_CUSTOMDRAW for checkbox/radio buttons (Common Controls 6): paints the
// whole control and returns CDRF_SKIPDEFAULT, or nullopt for other buttons.
// A BS_MULTILINE check box wraps its label, the box beside the first line.
[[nodiscard]] std::optional<LRESULT>
native_button_custom_draw(const NMHDR &header, const NativeThemeColors &colors,
                          HFONT font);
void paint_native_button(const DRAWITEMSTRUCT &draw,
                         const NativeThemeColors &colors, HFONT font,
                         std::optional<COLORREF> explicit_fill = std::nullopt);

} // namespace layerc::platform
