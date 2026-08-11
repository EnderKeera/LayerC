#pragma once

#include "layerc/core/template_matcher.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <variant>

namespace layerc::platform {

inline constexpr std::size_t max_template_asset_bytes = 4U * 1024U * 1024U;

struct VisualAssetError {
  std::string message;
};

struct ImportedTemplateAsset {
  layerc::core::TemplateReference reference;
  layerc::core::TemplateImage image;
  std::filesystem::path path;
};

using TemplateAssetResult =
    std::variant<ImportedTemplateAsset, VisualAssetError>;
using TemplateImageResult =
    std::variant<layerc::core::TemplateImage, VisualAssetError>;

[[nodiscard]] std::filesystem::path visual_asset_directory();
[[nodiscard]] std::filesystem::path
visual_asset_path(std::string_view sha256);

// Decodes, strips metadata by re-encoding, hashes the canonical PNG bytes, and
// atomically stores the result in the content-addressed asset directory.
[[nodiscard]] TemplateAssetResult
import_template_asset(const std::filesystem::path &source);
[[nodiscard]] TemplateAssetResult
store_template_asset(const layerc::core::TemplateImage &image);

// Verifies the file hash before decoding. A corrupt or replaced asset never
// reaches the matcher under the identity named by a profile.
[[nodiscard]] TemplateImageResult
load_template_asset(std::string_view sha256);

// Removes only well-formed hash-named PNGs absent from the supplied complete
// reference set. The caller must not invoke this after a partial profile scan.
[[nodiscard]] bool garbage_collect_visual_assets(
    std::span<const std::string> referenced_sha256);
// Enumerates the same top-level base profiles as LayerD and follows each
// sidecar's primary/backup/state recovery rules before deleting anything. An
// invalid active bundle aborts collection; an unrelated malformed base file
// or corrupt stale backup does not make every orphan immortal.
[[nodiscard]] bool garbage_collect_unreferenced_visual_assets();

} // namespace layerc::platform
