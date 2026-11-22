#pragma once

#include "../editor_appearance.hpp"

namespace layerc::editor {

[[nodiscard]] AppearanceLoad load_platform_appearance();
[[nodiscard]] std::optional<std::string> save_platform_appearance(const EditorAppearance &appearance);
[[nodiscard]] const char *appearance_replacement_prompt() noexcept;

} // namespace layerc::editor
