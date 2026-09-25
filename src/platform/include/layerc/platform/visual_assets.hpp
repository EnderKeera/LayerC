#pragma once

#include "layerc/core/template_matcher.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::platform {

inline constexpr std::size_t max_template_asset_bytes = 4U * 1024U * 1024U;

struct VisualAssetError {
  std::string message;
};

using TemplateImageResult =
    std::variant<layerc::core::TemplateImage, VisualAssetError>;
using DecodeTemplateRgba = TemplateImageResult (*)(std::vector<std::uint8_t> &);

// Reads a bounded, guarded file, checks the content hash, then decodes it.
[[nodiscard]] TemplateImageResult verified_template_from_bytes(
    const std::filesystem::path &path, std::string_view sha256,
    DecodeTemplateRgba decode_rgba);

// Linux profile-local loader for canonical 8-bit RGBA PNG bundles.
[[nodiscard]] TemplateImageResult
load_profile_template_asset(const std::filesystem::path &profile_path,
                            std::string_view sha256);

} // namespace layerc::platform
