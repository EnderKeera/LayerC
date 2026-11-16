#pragma once

#include "layerc/platform/visual_assets.hpp"
#include "layerc/platform/profile_transaction.hpp"

#include <span>
#include <vector>

namespace layerc::platform {

// Canonical content-addressed PNGs in this profile's asset store. Writes
// reuse the recovered one-writer stage/commit transaction. Calibration and
// undo may own unpublished references: interactive callers never collect assets.
[[nodiscard]] TemplateAssetResult store_profile_template_asset(
    const std::filesystem::path &profile_path,
    const layerc::core::TemplateImage &image);
using TemplateAssetBatchResult =
    std::variant<std::vector<ImportedTemplateAsset>, VisualAssetError>;
// A complete calibration batch is validated before publication and committed
// under one durable transaction. New images use canonical bytes; existing
// encoded identities are retained only after their digest and decoded image
// are reverified under writer ownership.
[[nodiscard]] TemplateAssetBatchResult store_profile_template_assets(
    const std::filesystem::path &profile_path,
    std::span<const layerc::core::TemplateImage> images);
[[nodiscard]] TemplateAssetResult import_profile_template_asset(
    const std::filesystem::path &profile_path,
    const std::filesystem::path &source);

// A duplicate owns the original encoded bytes, independently of the source
// namespace. Snapshot reads recover and acquire the source writer; publication
// is planned inside the destination bundle's transaction, never in a second one.
struct EncodedTemplateAsset {
  std::string asset_sha256;
  std::vector<std::byte> bytes;
};
using TemplateAssetSnapshotResult =
    std::variant<std::vector<EncodedTemplateAsset>, VisualAssetError>;
[[nodiscard]] TemplateAssetSnapshotResult snapshot_profile_template_assets(
    const std::filesystem::path &profile_path, const layerc::core::VisualProfile &visual,
    std::span<const EncodedTemplateAsset> retained = {});
[[nodiscard]] ProfileWriteResult plan_profile_template_copies(
    const std::filesystem::path &profile_path, std::span<const EncodedTemplateAsset> assets,
    std::vector<ProfileFileMutation> &mutations);
// A complete calibration batch as owned canonical bytes. Nothing is written:
// the editor's next bundle Save plans them into its own transaction. Images
// that keep an existing encoded identity are verified against an owned copy or
// this profile's stored asset and are not returned for publication.
[[nodiscard]] TemplateAssetSnapshotResult prepare_profile_template_assets(
    const std::filesystem::path &profile_path,
    std::span<const layerc::core::TemplateImage> images,
    std::span<const EncodedTemplateAsset> owned = {});

// Profile-local loader supplied by the platform backend.
[[nodiscard]] TemplateImageResult
load_profile_template_asset(const std::filesystem::path &profile_path,
                            std::string_view sha256);
// An editor-owned, possibly unpublished copy first; else the verified stored asset.
[[nodiscard]] TemplateImageResult
load_profile_template_asset(const std::filesystem::path &profile_path,
                            std::string_view sha256,
                            std::span<const EncodedTemplateAsset> owned);

} // namespace layerc::platform
