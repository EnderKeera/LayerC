#pragma once

#include "status_kind.hpp"

#include "layerc/platform/native_theme.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

namespace layerc::editor {

// A static control's text tone and status glyph (DesignSystem.md, Writing
// and StatusText), kept in its GWLP_USERDATA under a tag: statics never use
// that slot otherwise, and an untagged value reads as no style.
enum class TextTone : std::uint8_t { label, text, subtext };

struct StaticStyle {
  TextTone tone{TextTone::label};
  std::optional<layerc::platform::StatusGlyph> glyph;
  // The glyph and message hug the right edge (the status strip's states).
  bool align_right{};
};

inline constexpr LONG_PTR static_style_tag = 0x4C430000;
inline constexpr LONG_PTR static_style_tag_mask = 0x7FFF0000;

inline void set_static_style(const HWND control, const StaticStyle &style) {
  if (control == nullptr)
    return;
  const LONG_PTR glyph =
      style.glyph ? static_cast<LONG_PTR>(*style.glyph) + 1 : 0;
  SetWindowLongPtrW(control, GWLP_USERDATA,
                    static_style_tag | (static_cast<LONG_PTR>(style.tone) << 4U) |
                        (style.align_right ? LONG_PTR{0x40} : 0) | glyph);
  InvalidateRect(control, nullptr, TRUE);
}

[[nodiscard]] inline std::optional<StaticStyle> static_style(const HWND control) {
  if (control == nullptr)
    return std::nullopt;
  const LONG_PTR value = GetWindowLongPtrW(control, GWLP_USERDATA);
  if ((value & static_style_tag_mask) != static_style_tag)
    return std::nullopt;
  StaticStyle style;
  style.tone = static_cast<TextTone>((value >> 4U) & 0x3);
  style.align_right = (value & 0x40) != 0;
  if (const LONG_PTR glyph = value & 0xF; glyph != 0)
    style.glyph = static_cast<layerc::platform::StatusGlyph>(glyph - 1);
  return style;
}

inline void set_text_tone(const HWND control, const TextTone tone) {
  set_static_style(control, {.tone = tone, .glyph = std::nullopt});
}

// A status line is an owner-drawn static (SS_OWNERDRAW) whose text is the
// message and whose accessible name stays that text. With a glyph the
// message is `text`; without one it reads as a hint in `subtext`.
inline void set_status_line(const HWND control,
                            const std::optional<layerc::platform::StatusGlyph> glyph,
                            const std::wstring_view text) {
  if (control == nullptr)
    return;
  const std::wstring value(text);
  SetWindowTextW(control, value.c_str());
  set_static_style(control, {.tone = glyph ? TextTone::text : TextTone::subtext,
                             .glyph = glyph});
}

// The drawn glyph of a status kind (DesignSystem.md, StatusText); `none`
// draws nothing.
[[nodiscard]] inline std::optional<layerc::platform::StatusGlyph>
status_glyph(const StatusKind kind) noexcept {
  using layerc::platform::StatusGlyph;
  switch (kind) {
  case StatusKind::success:
    return StatusGlyph::yes;
  case StatusKind::failure:
    return StatusGlyph::no;
  case StatusKind::warning:
    return StatusGlyph::warning;
  case StatusKind::information:
    return StatusGlyph::accent_2;
  case StatusKind::none:
    break;
  }
  return std::nullopt;
}

// A status strip state (DesignSystem.md, StatusStrip): the glyph of its kind,
// then the message in the strip's `secondary` text, right-aligned. The text
// stays the accessible name.
inline void set_strip_status(const HWND control, const StatusKind kind,
                             const std::wstring_view text) {
  if (control == nullptr)
    return;
  const auto glyph = status_glyph(kind);
  std::wstring current(
      static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
  current.resize(static_cast<std::size_t>(GetWindowTextW(
      control, current.data(), static_cast<int>(current.size()))));
  const auto style = static_style(control);
  // A refresh that changes nothing must not repaint the strip.
  if (current == text && style && style->align_right && style->glyph == glyph)
    return;
  const std::wstring value(text);
  SetWindowTextW(control, value.c_str());
  set_static_style(control, {.tone = TextTone::label,
                             .glyph = glyph,
                             .align_right = true});
}

} // namespace layerc::editor
