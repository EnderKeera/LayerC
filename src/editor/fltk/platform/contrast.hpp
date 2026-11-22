#pragma once

#include "editor_appearance.hpp"

namespace layerc::editor {

struct ContrastPalette {
  std::array<RoleColors, editor_role_count> roles;
  EditorTokens tokens;
};

[[nodiscard]] std::optional<ContrastPalette> platform_contrast_palette();

} // namespace layerc::editor
