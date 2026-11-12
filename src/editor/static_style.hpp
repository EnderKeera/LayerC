#pragma once

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

// The glyph a status strip state leads with (DesignSystem.md, StatusStrip),
// from the LayerD and visual recognition messages as written: running or
// active is `yes`, a fallback `warning`, a stop or fault `no`, and waiting
// or switched off `accent_2`.
[[nodiscard]] inline layerc::platform::StatusGlyph
status_strip_glyph(const std::wstring_view text) {
  using layerc::platform::StatusGlyph;
  const auto has = [text](const std::wstring_view part) {
    return text.find(part) != std::wstring_view::npos;
  };
  if (has(L"not running") || has(L"blocked") || has(L"quarantined") ||
      has(L"missing") || has(L"unavailable"))
    return StatusGlyph::no;
  if (has(L"connected") || (has(L"active") && !has(L"inactive")))
    return StatusGlyph::yes;
  if (has(L"fallback"))
    return StatusGlyph::warning;
  return StatusGlyph::accent_2;
}

// A status strip state: its glyph, then the message in the strip's
// `secondary` text, right-aligned. The text stays the accessible name.
inline void set_strip_status(const HWND control, const std::wstring_view text) {
  if (control == nullptr)
    return;
  const auto glyph = status_strip_glyph(text);
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
