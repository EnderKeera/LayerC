#include "editor_color.hpp"
#include "layerc/platform/native_theme.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// The design system's color contracts (DesignSystem.md, "Where each color
// may go"), asserted for every preset, plus the drawn status glyphs and the
// contrast-theme seam.
namespace {

using layerc::platform::EditorControlRole;
using layerc::platform::EditorTheme;
using layerc::platform::NativeThemeColors;
using layerc::test::expect;

struct Preset {
  EditorTheme theme;
  std::string_view name;
  std::uint32_t accent;
  std::uint32_t accent_2;
};

constexpr std::array presets{
    Preset{EditorTheme::copper, "Copper", layerc::platform::copper_accent,
           layerc::editor::accent_2_blue},
    Preset{EditorTheme::teal, "Teal", layerc::platform::teal_accent,
           layerc::editor::accent_2_violet},
    Preset{EditorTheme::plum, "Plum", layerc::platform::plum_accent,
           layerc::editor::accent_2_blue},
    Preset{EditorTheme::dark, "Slate", layerc::platform::slate_accent,
           layerc::editor::accent_2_violet},
};

NativeThemeColors preset_colors(const Preset &preset,
                                const EditorControlRole role) {
  return layerc::platform::preset_theme_colors(preset.theme, 0, role, {});
}

void expect_floor(const Preset &preset, const std::string_view pair,
                  const COLORREF foreground, const COLORREF background,
                  const double minimum) {
  const double ratio = layerc::platform::contrast_ratio(foreground, background);
  expect(ratio >= minimum,
         std::string(preset.name) + ": " + std::string(pair) + " contrast " +
             std::to_string(ratio) + " is below " + std::to_string(minimum));
}

// A glyph's ink at a point on its 12-unit grid.
bool inked(const HDC dc, const RECT &box, const double x, const double y,
           const COLORREF ink) {
  const int size = box.right - box.left;
  return GetPixel(dc, box.left + static_cast<int>(x * size / 12.0),
                  box.top + static_cast<int>(y * size / 12.0)) == ink;
}

} // namespace

void editor_theme_contrast_contracts() {
  layerc::platform::set_native_high_contrast_override(false);
  for (const auto &preset : presets) {
    const auto window = preset_colors(preset, EditorControlRole::window);
    const auto edit = preset_colors(preset, EditorControlRole::edit);
    const auto button = preset_colors(preset, EditorControlRole::button);
    expect(window.accent == layerc::platform::native_color_ref(preset.accent),
           std::string(preset.name) + ": the preset accent");
    expect(window.accent_2 ==
               layerc::platform::native_color_ref(preset.accent_2),
           std::string(preset.name) + ": accent_2 keeps clear of the accent");
    expect(window.selection ==
               layerc::platform::blend_color(window.fill, window.accent, 60),
           std::string(preset.name) + ": selection blends 60/255 of accent");
    const std::array<std::pair<std::string_view, COLORREF>, 4> grounds{{
        {"background", window.fill},
        {"input", edit.fill},
        {"panel", button.fill},
        {"selection", window.selection},
    }};
    for (const auto &[ground_name, ground] : grounds) {
      const bool selection = ground_name == "selection";
      const auto on = [&](const std::string_view color) {
        return std::string(color) + " on " + std::string(ground_name);
      };
      expect_floor(preset, on("text"), window.text, ground, 4.5);
      expect_floor(preset, on("secondary"), window.secondary, ground, 4.5);
      expect_floor(preset, on("yes"), window.yes, ground, 4.5);
      expect_floor(preset, on("warning"), window.warning, ground, 4.5);
      expect_floor(preset, on("accent_2"), window.accent_2, ground, 4.5);
      // On a selected row only the glyph keeps `no`: 3:1 for a graphic.
      expect_floor(preset, on("no"), window.no, ground, selection ? 3.0 : 4.5);
      expect_floor(preset, on("accent mark"), window.accent, ground, 3.0);
      if (selection)
        continue;
      expect_floor(preset, on("subtext"), window.subtext, ground, 4.5);
      expect_floor(preset, on("verbose"), window.verbose, ground, 4.5);
      expect_floor(preset, on("disabled"), window.disabled, ground, 3.0);
    }
    expect_floor(preset, "border on background", window.border, window.fill,
                 3.0);
    expect_floor(preset, "border on input", edit.border, edit.fill, 3.0);
    expect_floor(preset, "on_accent on accent", window.on_accent,
                 window.accent, 4.5);
    for (const auto status : {window.yes, window.no, window.warning})
      expect_floor(preset, "on_status on a status fill", window.on_status,
                   status, 4.5);
  }
  // The token values themselves (DesignSystem.md, Token values).
  const auto copper = preset_colors(presets[0], EditorControlRole::window);
  expect(copper.subtext == RGB(0xA3, 0xA3, 0xA3) &&
             copper.verbose == RGB(0x9A, 0x9A, 0x9A) &&
             copper.yes == RGB(0x00, 0xE6, 0x76) &&
             copper.no == RGB(0xFF, 0x5A, 0x5A) &&
             copper.warning == RGB(0xFF, 0xD7, 0x40) &&
             copper.on_status == RGB(0x14, 0x14, 0x14),
         "the v2 text and status tokens");
  // The System preset's accent_2 follows the same hue rule: the default
  // Windows blue takes violet, an orange accent blue.
  expect(layerc::editor::rgb_accent_2(0x0078D4) ==
                 layerc::editor::accent_2_violet &&
             layerc::editor::rgb_accent_2(0xE81123) ==
                 layerc::editor::accent_2_blue,
         "accent_2 picks the hue away from any accent");
  layerc::platform::set_native_high_contrast_override(std::nullopt);
}

void editor_theme_high_contrast_contracts() {
  // The seam stands in for SPI_GETHIGHCONTRAST; the system setting is never
  // changed.
  layerc::platform::EditorAppearanceOverrides overrides{};
  overrides.fields = layerc::platform::editor_fill;
  overrides.fill = 0x123456;
  layerc::platform::set_native_high_contrast_override(true);
  expect(layerc::platform::native_high_contrast(), "the seam forces it on");
  const auto window = layerc::platform::native_theme_colors(
      EditorTheme::copper, 0, EditorControlRole::window, overrides);
  const auto button = layerc::platform::native_theme_colors(
      EditorTheme::copper, 0, EditorControlRole::button, {});
  expect(window.high_contrast && button.high_contrast,
         "contrast-theme colors are marked");
  expect(window.fill == GetSysColor(COLOR_WINDOW),
         "a contrast theme sets the overrides aside");
  expect(window.text == GetSysColor(COLOR_WINDOWTEXT) &&
             window.secondary == window.text && window.subtext == window.text &&
             window.yes == window.text && window.no == window.text &&
             window.border == window.text,
         "text, hints, status and borders take COLOR_WINDOWTEXT");
  expect(button.fill == GetSysColor(COLOR_BTNFACE) &&
             button.text == GetSysColor(COLOR_BTNTEXT),
         "buttons take COLOR_BTNFACE and COLOR_BTNTEXT");
  expect(window.accent == GetSysColor(COLOR_HIGHLIGHT) &&
             window.selection == window.accent &&
             window.on_accent == GetSysColor(COLOR_HIGHLIGHTTEXT),
         "accent and selection take COLOR_HIGHLIGHT");
  expect(window.disabled == GetSysColor(COLOR_GRAYTEXT) &&
             window.divider == window.disabled,
         "disabled text and outlines take COLOR_GRAYTEXT");
  expect(window.hover == window.fill && window.pressed == window.fill,
         "hover and pressed keep the fill");
  // Appearance still validates the overrides against the preset.
  const auto preset = layerc::platform::preset_theme_colors(
      EditorTheme::copper, 0, EditorControlRole::window, overrides);
  expect(!preset.high_contrast &&
             preset.fill == layerc::platform::native_color_ref(0x123456),
         "the saved overrides stay intact");
  layerc::platform::set_native_high_contrast_override(false);
  expect(!layerc::platform::native_high_contrast() &&
             !layerc::platform::native_theme_colors(
                  EditorTheme::copper, 0, EditorControlRole::window, {})
                  .high_contrast,
         "the seam forces it off");
  layerc::platform::set_native_high_contrast_override(std::nullopt);
}

void editor_status_glyph_contracts() {
  const HDC screen = GetDC(nullptr);
  const HDC dc = CreateCompatibleDC(screen);
  const HBITMAP bitmap = CreateCompatibleBitmap(screen, 96, 96);
  ReleaseDC(nullptr, screen);
  expect(dc != nullptr && bitmap != nullptr, "a glyph canvas");
  if (dc == nullptr || bitmap == nullptr)
    return;
  const HGDIOBJ previous = SelectObject(dc, bitmap);
  constexpr UINT dpi = 192;
  const COLORREF ground = RGB(0x1F, 0x1F, 0x1F);
  const COLORREF ink = RGB(0x00, 0xE6, 0x76);
  struct Probe {
    layerc::platform::StatusGlyph glyph;
    std::string_view name;
    double x, y; // inside a stroke or square
  };
  constexpr std::array probes{
      Probe{layerc::platform::StatusGlyph::yes, "yes", 5.0, 8.6},
      Probe{layerc::platform::StatusGlyph::no, "no", 6.0, 6.0},
      Probe{layerc::platform::StatusGlyph::warning, "warning", 6.0, 4.0},
      Probe{layerc::platform::StatusGlyph::accent_2, "accent_2", 2.0, 9.0},
  };
  const int size = layerc::platform::status_glyph_size(dpi);
  expect(size >= 24, "status glyphs are 12 px at 96 DPI, scaled");
  for (const auto &probe : probes) {
    const RECT bounds{0, 0, 96, 96};
    layerc::platform::fill_solid_rect(dc, bounds, ground);
    const RECT box =
        layerc::platform::paint_status_glyph(dc, bounds, probe.glyph, ink, dpi);
    expect(box.right - box.left == std::min(size, 96) &&
               box.bottom - box.top == box.right - box.left,
           std::string(probe.name) + ": a square glyph box");
    expect(inked(dc, box, probe.x, probe.y, ink),
           std::string(probe.name) + ": drawn in its status color");
    expect(!inked(dc, box, 0.2, 0.2, ink),
           std::string(probe.name) + ": the corner stays clear");
  }
  const auto colors = layerc::platform::preset_theme_colors(
      EditorTheme::copper, 0, EditorControlRole::window, {});
  expect(layerc::platform::status_color(
             colors, layerc::platform::StatusGlyph::warning) == colors.warning &&
             layerc::platform::status_color(
                 colors, layerc::platform::StatusGlyph::accent_2) ==
                 colors.accent_2,
         "each glyph has its status color");
  SelectObject(dc, previous);
  DeleteObject(bitmap);
  DeleteDC(dc);
}
