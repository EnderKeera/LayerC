#pragma once

#include "layerc/core/template_matcher.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <variant>

namespace layerc::platform {

inline constexpr std::size_t max_template_asset_bytes = 4U * 1024U * 1024U;

struct VisualAssetError {
  std::string message;
};

// Content identity for a stored template asset. The change-journal USN
// invalidates a cached decode even when a repair preserves size, timestamp,
// and file index.
struct VisualAssetSignature {
  bool present{};
  std::uint32_t volume_serial{};
  std::uint64_t file_index{};
  std::uint64_t file_size{};
  std::uint64_t last_write_ticks{};
  // NTFS change-journal USN. Zero means unavailable (non-NTFS or no journal).
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

using TemplateAssetResult =
    std::variant<ImportedTemplateAsset, VisualAssetError>;
using TemplateImageResult =
    std::variant<layerc::core::TemplateImage, VisualAssetError>;

[[nodiscard]] std::filesystem::path visual_asset_directory();
[[nodiscard]] std::filesystem::path
visual_asset_path(std::string_view sha256);

// File identity of a content-addressed asset. Uses NTFS USN when available.
[[nodiscard]] VisualAssetSignature asset_signature(std::string_view sha256);

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

// Linux profile-local loader: verifies the content hash and decodes the
// canonical 8-bit, non-interlaced RGBA PNG form emitted by LayerC bundles.
// Implemented by the Linux platform asset TU (miniz decoder), consumed by
// the daemon visual service as a verified TemplateImageResult — the daemon
// owns no parser or storage policy here.
[[nodiscard]] TemplateImageResult
load_profile_template_asset(const std::filesystem::path &profile_path,
                            std::string_view sha256);

// Removes only well-formed hash-named PNGs absent from the supplied complete
// reference set. The caller must not invoke this after a partial profile scan.
[[nodiscard]] VisualAssetGcReport garbage_collect_visual_assets(
    std::span<const std::string> referenced_sha256);
// Enumerates the same top-level base profiles as LayerD and follows each
// sidecar's primary/backup/state recovery rules before deleting anything. An
// invalid active bundle aborts collection; an unrelated malformed base file
// or corrupt stale backup does not make every orphan immortal.
[[nodiscard]] VisualAssetGcReport
garbage_collect_unreferenced_visual_assets();

} // namespace layerc::platform
