#pragma once

#include <cstdint>
#include <string>

namespace layerc::editor {

// The Overlay minimums a bar's own colors are checked against
// (DesignSystem.md, OverlayBar): text on Fill and on Background, the border
// on Background.
inline constexpr double overlay_text_minimum = 4.5;
inline constexpr double overlay_mark_minimum = 3.0;

// The OverlayBar contrast line for a bar's packed 0xRRGGBB colors, without the
// warning mark the surface draws: "Low contrast. Text is 1.7:1 on Fill and
// 1.9:1 on Background; aim for 4.5:1. Border is 1.4:1 on Background; aim for
// 3:1." Text pairs count only while the bar shows text. Ratios round down to
// one decimal, so a printed ratio never reads as passing. UTF-8; empty when
// every pair passes. It never changes the profile or blocks a save.
[[nodiscard]] std::string overlay_contrast_line(std::uint32_t fill, std::uint32_t background,
                                                std::uint32_t border, std::uint32_t text,
                                                bool show_text);

} // namespace layerc::editor
