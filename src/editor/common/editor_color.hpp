#pragma once

#include <cstdint>

namespace layerc::editor {

// The two values of `accent_2`; rgb_accent_2 picks the one whose hue keeps
// clear of the accent.
inline constexpr std::uint32_t accent_2_blue = 0x60B0FF;
inline constexpr std::uint32_t accent_2_violet = 0xB99BFF;

// WCAG 2 relative luminance, 0 to 1.
[[nodiscard]] double rgb_relative_luminance(std::uint32_t color) noexcept;
// WCAG 2 contrast ratio of two colors, 1 to 21, in either order.
[[nodiscard]] double rgb_contrast_ratio(std::uint32_t first, std::uint32_t second) noexcept;
// Hue in degrees, [0, 360); 0 for a gray.
[[nodiscard]] double rgb_hue_degrees(std::uint32_t color) noexcept;
// `accent_2` for an accent: violet when the accent's hue lies within 45
// degrees of the blue's (Teal, Slate, the default Windows blue), else blue.
[[nodiscard]] std::uint32_t rgb_accent_2(std::uint32_t accent) noexcept;
// Ink on an accent fill: #141414 on light accents (weighted luma
// r*299 + g*587 + b*114 >= 140000), else white.
[[nodiscard]] std::uint32_t rgb_on_accent(std::uint32_t accent) noexcept;

} // namespace layerc::editor
