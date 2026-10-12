#pragma once

#include "layerc/core/template_matcher.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
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

struct ImportedTemplateAsset {
  layerc::core::TemplateReference reference;
  layerc::core::TemplateImage image;
  std::filesystem::path path;
};
using TemplateAssetResult = std::variant<ImportedTemplateAsset, VisualAssetError>;

// Bounded, guarded read of one template asset; nullopt when the file is
// missing, empty, or larger than max_template_asset_bytes.
[[nodiscard]] std::optional<std::vector<std::uint8_t>>
read_template_asset_bytes(const std::filesystem::path &path);

// Reads a bounded, guarded file, checks the content hash, then decodes it
// with the shared template PNG codec.
[[nodiscard]] TemplateImageResult
verified_template_from_bytes(const std::filesystem::path &path,
                             std::string_view sha256);

} // namespace layerc::platform
