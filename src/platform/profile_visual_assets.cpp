#include "layerc/platform/profile_visual_assets.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/template_png.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace layerc::platform {
namespace {
std::optional<std::string> validate_encoded_template(const EncodedTemplateAsset &asset) {
  if (asset.bytes.empty() || asset.bytes.size() > max_template_asset_bytes ||
      layerc::core::sha256(asset.bytes) != asset.asset_sha256)
    return "template snapshot hash or size is invalid";
  const auto *raw = reinterpret_cast<const std::uint8_t *>(asset.bytes.data());
  auto image = decode_template_png(std::span(raw, asset.bytes.size()));
  if (const auto *error = std::get_if<VisualAssetError>(&image)) return error->message;
  if (!layerc::core::template_image_has_structure(std::get<layerc::core::TemplateImage>(image)))
    return "template snapshot lacks recognizable visible structure";
  return std::nullopt;
}

TemplateImageResult decode_owned_template(const EncodedTemplateAsset &asset) {
  if (auto error = validate_encoded_template(asset)) return VisualAssetError{std::move(*error)};
  const auto *raw = reinterpret_cast<const std::uint8_t *>(asset.bytes.data());
  auto image = decode_template_png(std::span(raw, asset.bytes.size()));
  if (const auto *error = std::get_if<VisualAssetError>(&image)) return *error;
  return std::get<layerc::core::TemplateImage>(std::move(image));
}
} // namespace

TemplateAssetSnapshotResult snapshot_profile_template_assets(
    const std::filesystem::path &profile_path, const layerc::core::VisualProfile &visual,
    const std::span<const EncodedTemplateAsset> retained) {
  std::vector<EncodedTemplateAsset> assets;
  std::optional<std::string> fault;
  const auto snapshot = [&](std::vector<ProfileFileMutation> &) -> ProfileWriteResult {
    for (const auto &observation : visual.observations) {
      const auto *state = std::get_if<layerc::core::TemplateState>(&observation);
      if (!state) continue;
      for (const auto &reference : state->references) {
        if (std::ranges::find(assets, reference.asset_sha256,
                              &EncodedTemplateAsset::asset_sha256) != assets.end()) continue;
        EncodedTemplateAsset asset{.asset_sha256 = reference.asset_sha256, .bytes = {}};
        const auto owned = std::ranges::find(retained, reference.asset_sha256,
                                            &EncodedTemplateAsset::asset_sha256);
        if (owned != retained.end()) asset.bytes = owned->bytes;
        else {
          const auto bytes = read_template_asset_bytes(
              profile_visual_asset_path(profile_path, reference.asset_sha256));
          if (!bytes) {
            fault = "a referenced template is missing, unreadable or unsafe";
            return {ProfileWriteError::io_failed};
          }
          const auto contents = std::as_bytes(std::span(*bytes));
          asset.bytes.assign(contents.begin(), contents.end());
        }
        if (auto error = validate_encoded_template(asset)) {
          fault = std::move(*error); return {ProfileWriteError::invalid_input};
        }
        assets.push_back(std::move(asset));
      }
    }
    return {};
  };
  std::vector<ProfileFileMutation> unused;
  const auto result = profile_path.empty()
      ? snapshot(unused) : commit_profile_transaction(profile_path.parent_path(), snapshot);
  if (!result)
    return VisualAssetError{fault.value_or(profile_write_error_text(*result.failure))};
  return assets;
}

ProfileWriteResult plan_profile_template_copies(
    const std::filesystem::path &profile_path, const std::span<const EncodedTemplateAsset> assets,
    std::vector<ProfileFileMutation> &mutations) {
  for (const auto &asset : assets) {
    if (validate_encoded_template(asset)) return {ProfileWriteError::invalid_input};
    const auto destination = profile_visual_asset_path(profile_path, asset.asset_sha256);
    if (destination.empty()) return {ProfileWriteError::invalid_input};
    if (std::ranges::none_of(mutations, [&](const auto &mutation) {
          return mutation.target == destination;
        }))
      mutations.push_back({.target = destination, .contents = asset.bytes});
  }
  return {};
}

TemplateAssetSnapshotResult prepare_profile_template_assets(
    const std::filesystem::path &profile_path,
    const std::span<const layerc::core::TemplateImage> images,
    const std::span<const EncodedTemplateAsset> owned) {
  if (images.size() > layerc::core::max_template_references)
    return VisualAssetError{"too many template assets in one calibration"};
  std::vector<EncodedTemplateAsset> prepared;
  prepared.reserve(images.size());
  for (const auto &image : images) {
    if (!layerc::core::template_image_has_structure(image))
      return VisualAssetError{
          "template needs visible light/color variation so its position can be recognized"};
    auto encoded = encode_template_png(image);
    if (const auto *error = std::get_if<VisualAssetError>(&encoded)) return *error;
    const auto content = std::as_bytes(std::span(std::get<std::vector<std::uint8_t>>(encoded)));
    const auto canonical = layerc::core::sha256(content);
    if (image.asset_sha256.empty() || image.asset_sha256 == canonical) {
      if (std::ranges::find(prepared, canonical, &EncodedTemplateAsset::asset_sha256) ==
          prepared.end())
        prepared.push_back({.asset_sha256 = canonical,
                            .bytes = std::vector<std::byte>(content.begin(), content.end())});
      continue;
    }
    // Valid RGBA files may have other compression or ancillary metadata. Keep
    // their encoded identity only when those bytes decode to these pixels.
    const auto owner = std::ranges::find(owned, image.asset_sha256,
                                         &EncodedTemplateAsset::asset_sha256);
    auto existing = owner != owned.end() ? decode_owned_template(*owner)
                    : profile_path.empty()
                        ? TemplateImageResult{VisualAssetError{
                              "an existing template is not available to this draft"}}
                        : load_profile_template_asset(profile_path, image.asset_sha256);
    if (const auto *error = std::get_if<VisualAssetError>(&existing)) return *error;
    const auto &stored = std::get<layerc::core::TemplateImage>(existing);
    if (stored.width != image.width || stored.height != image.height ||
        stored.pixels != image.pixels || stored.alpha != image.alpha)
      return VisualAssetError{"an existing template does not match the calibration pixels or mask"};
  }
  return prepared;
}

TemplateImageResult
load_profile_template_asset(const std::filesystem::path &profile_path,
                            const std::string_view sha256) {
  return verified_template_from_bytes(
      profile_visual_asset_path(profile_path, sha256), sha256);
}

TemplateImageResult
load_profile_template_asset(const std::filesystem::path &profile_path,
                            const std::string_view sha256,
                            const std::span<const EncodedTemplateAsset> owned) {
  const auto owner = std::ranges::find(owned, sha256, &EncodedTemplateAsset::asset_sha256);
  if (owner == owned.end()) return load_profile_template_asset(profile_path, sha256);
  auto image = decode_owned_template(*owner);
  if (auto *decoded = std::get_if<layerc::core::TemplateImage>(&image))
    decoded->asset_sha256 = std::string(sha256);
  return image;
}

} // namespace layerc::platform
