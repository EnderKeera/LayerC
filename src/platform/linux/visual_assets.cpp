#include "layerc/platform/visual_assets_linux.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_file_guard.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/template_png.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace layerc::platform {

TemplateAssetBatchResult store_profile_template_assets(
    const std::filesystem::path &profile_path,
    const std::span<const layerc::core::TemplateImage> images) {
  if (profile_path.empty() || profile_path.extension() != ".json" ||
      !profile_transaction_scope(profile_path.parent_path()))
    return VisualAssetError{"template destination is outside the profile store"};
  if (images.size() > layerc::core::max_template_references)
    return VisualAssetError{"too many template assets in one calibration"};
  std::vector<ImportedTemplateAsset> prepared;
  std::vector<ProfileFileMutation> publications;
  std::vector<std::size_t> retained;
  std::optional<std::string> retained_fault;
  prepared.reserve(images.size());
  publications.reserve(images.size());
  retained.reserve(images.size());
  for (const auto &image : images) {
    if (!layerc::core::template_image_has_structure(image))
      return VisualAssetError{
          "template needs visible light/color variation so its position can be recognized"};
    auto encoded = encode_template_png(image);
    if (const auto *error = std::get_if<VisualAssetError>(&encoded)) return *error;
    const auto &bytes = std::get<std::vector<std::uint8_t>>(encoded);
    const auto content = std::as_bytes(std::span(bytes));
    const auto canonical_digest = layerc::core::sha256(content);
    const auto digest = image.asset_sha256.empty() ? canonical_digest : image.asset_sha256;
    const auto destination = profile_visual_asset_path(profile_path, digest);
    if (destination.empty()) return VisualAssetError{"content-addressed asset path is invalid"};
    prepared.push_back({.reference = {.asset_sha256 = digest},
                        .image = {}, .path = destination});
    if (digest != canonical_digest) {
      retained.push_back(prepared.size() - 1U);
      continue;
    }
    if (std::ranges::none_of(publications, [&](const auto &mutation) {
          return mutation.target == destination;
        }))
      publications.push_back({.target = destination,
          .contents = std::vector<std::byte>(content.begin(), content.end())});
  }
  if (images.empty()) return prepared;
  const auto write = commit_profile_transaction(
      profile_path.parent_path(), [&](std::vector<ProfileFileMutation> &mutations) {
        const auto refuse_retained = [&](std::string reason) {
          retained_fault = std::move(reason);
          return ProfileWriteResult{ProfileWriteError::io_failed};
        };
        // Valid RGBA files may have other compression or ancillary metadata.
        // Keep their encoded identity, but verify every borrowed image under
        // the recovered writer ownership before staging any new content.
        for (const auto index : retained) {
          const auto &asset = prepared[index];
          const auto parent = asset.path.parent_path();
          if (!file_guard_regular_file_exists(asset.path).value_or(false) ||
              file_guard_is_symlink(parent) || !file_guard_owned_directory(parent) ||
              file_guard_is_symlink(parent.parent_path()) ||
              !file_guard_owned_directory(parent.parent_path()))
            return refuse_retained("an existing template path is unsafe or missing");
          auto verified = load_profile_template_asset(profile_path, asset.reference.asset_sha256);
          if (const auto *error = std::get_if<VisualAssetError>(&verified))
            return refuse_retained(error->message);
          const auto &existing = std::get<layerc::core::TemplateImage>(verified);
          const auto &proposed = images[index];
          if (existing.width != proposed.width || existing.height != proposed.height ||
              existing.pixels != proposed.pixels || existing.alpha != proposed.alpha)
            return refuse_retained("an existing template does not match the calibration pixels or mask");
        }
        for (auto &publication : publications) {
          const auto &destination = publication.target;
          if (!file_guard_regular_file_exists(destination).has_value())
            return ProfileWriteResult{ProfileWriteError::io_failed};
          const auto existing = read_durable_file(destination, max_template_asset_bytes);
          const auto parent = destination.parent_path();
          if (existing.has_value() && existing.bytes == publication.contents &&
              !file_guard_is_symlink(parent) && file_guard_owned_directory(parent) &&
              !file_guard_is_symlink(parent.parent_path()) &&
              file_guard_owned_directory(parent.parent_path()))
            continue;
          if (existing.status == FileReadStatus::io_error)
            return ProfileWriteResult{ProfileWriteError::io_failed};
          mutations.push_back(std::move(publication));
        }
        return ProfileWriteResult{};
      });
  if (!write) {
    if (retained_fault) return VisualAssetError{std::move(*retained_fault)};
    return VisualAssetError{std::string("template asset publication failed or is unconfirmed: ") +
                            profile_write_error_text(*write.failure)};
  }
  for (std::size_t index{}; index < images.size(); ++index) {
    prepared[index].image = images[index];
    prepared[index].image.asset_sha256 = prepared[index].reference.asset_sha256;
  }
  return prepared;
}

TemplateAssetResult store_profile_template_asset(
    const std::filesystem::path &profile_path,
    const layerc::core::TemplateImage &image) {
  // Single imports normalize arbitrary decoded PNGs; calibration batches
  // instead verify the digest that their complete validated module references.
  auto canonical = image;
  canonical.asset_sha256.clear();
  auto result = store_profile_template_assets(profile_path, std::span(&canonical, 1));
  if (const auto *error = std::get_if<VisualAssetError>(&result)) return *error;
  return std::move(std::get<std::vector<ImportedTemplateAsset>>(result).front());
}

TemplateAssetResult import_profile_template_asset(
    const std::filesystem::path &profile_path,
    const std::filesystem::path &source) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(source, error)) || error)
    return VisualAssetError{"PNG source is not a plain regular file"};
  const auto bytes = read_template_asset_bytes(source);
  if (!bytes) return VisualAssetError{"PNG is missing, unreadable, empty, or larger than 4 MiB"};
  auto decoded = decode_template_png(*bytes);
  if (const auto *error = std::get_if<VisualAssetError>(&decoded)) return *error;
  return store_profile_template_asset(profile_path,
      std::get<layerc::core::TemplateImage>(decoded));
}

} // namespace layerc::platform
