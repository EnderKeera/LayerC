#pragma once

#include "layerc/platform/visual_assets.hpp"

namespace layerc::platform {

// Linux profile-local loader.
[[nodiscard]] TemplateImageResult
load_profile_template_asset(const std::filesystem::path &profile_path,
                            std::string_view sha256);

} // namespace layerc::platform
