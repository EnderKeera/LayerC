#pragma once

#include <array>
#include <cstdint>

namespace layerc::editor {

inline constexpr std::array text_scale_choices{100, 115, 130};

[[nodiscard]] constexpr int normalize_editor_text_scale(std::uint32_t percent) noexcept {
  for (const int choice : text_scale_choices)
    if (percent == static_cast<std::uint32_t>(choice)) return choice;
  return text_scale_choices.front();
}

} // namespace layerc::editor
