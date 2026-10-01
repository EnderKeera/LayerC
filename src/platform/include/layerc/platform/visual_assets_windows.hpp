#pragma once

#include "layerc/platform/visual_assets.hpp"

namespace layerc::platform {

// Content identity for a stored template asset, including an NTFS change USN.
struct VisualAssetSignature {
  bool present{};
  std::uint32_t volume_serial{};
  std::uint64_t file_index{};
  std::uint64_t file_size{};
  std::uint64_t last_write_ticks{};
  std::uint64_t change_usn{};

  bool operator==(const VisualAssetSignature &) const = default;
};

struct VisualAssetGcReport {
  std::size_t deleted{};
  std::size_t retained{};
  std::filesystem::path blocked_by;
  std::string fault;

  [[nodiscard]] bool completed() const noexcept { return fault.empty(); }
};

struct ImportedTemplateAsset {
  layerc::core::TemplateReference reference;
  layerc::core::TemplateImage image;
  std::filesystem::path path;
};
using TemplateAssetResult = std::variant<ImportedTemplateAsset, VisualAssetError>;

[[nodiscard]] std::filesystem::path visual_asset_directory();
[[nodiscard]] std::filesystem::path visual_asset_path(std::string_view sha256);
[[nodiscard]] VisualAssetSignature asset_signature(std::string_view sha256);
[[nodiscard]] TemplateAssetResult
import_template_asset(const std::filesystem::path &source);
[[nodiscard]] TemplateAssetResult
store_template_asset(const layerc::core::TemplateImage &image);
[[nodiscard]] TemplateImageResult load_template_asset(std::string_view sha256);
// Offline maintenance only: callers must have released all interactive
// document/history and calibration owners. Their unpublished references are
// not represented by the persisted sidecars. Production edit/calibration
// paths deliberately never invoke this operation. The recovered store writer
// boundary owns the complete scan and deletion; a caller cannot supply a stale
// externally prepared reference set.
[[nodiscard]] VisualAssetGcReport garbage_collect_unreferenced_visual_assets();

} // namespace layerc::platform
