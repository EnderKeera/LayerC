#pragma once

#include "platform/contrast.hpp"
#include "layerc/platform/native_theme.hpp"

namespace layerc::editor {

[[nodiscard]] ContrastPalette make_windows_contrast_palette(
    const std::array<platform::NativeThemeColors, editor_role_count> &roles) noexcept;

} // namespace layerc::editor
