#include "layerc/platform/profile_bundle_load.hpp"

#include "layerc/core/json.hpp"
#include "layerc/platform/durable_file.hpp"

#include <utility>

namespace layerc::platform {

bool source_schema_before(const std::string_view source,
                          const std::uint32_t current_schema) {
  const auto parsed = layerc::core::json::parse(source);
  const auto *root = std::get_if<layerc::core::json::Value>(&parsed);
  if (root == nullptr || !root->is_object()) return false;
  const auto *version = root->find("schemaVersion");
  return version != nullptr && version->is_number() &&
         version->as_number() < static_cast<double>(current_schema);
}

namespace {

// True when the loaded visual sidecar's source file uses an older schema.
bool visual_sidecar_is_legacy(const VisualSidecarLoad &visual) {
  if (visual.source_path().empty()) return false;
  const auto source = read_durable_text(visual.source_path(),
                                        layerc::core::max_visual_json_bytes);
  return source && source_schema_before(*source,
                                        layerc::core::visual_schema_version);
}

} // namespace

std::variant<ProfileBundleLoad, ProfileBundleLoadError>
load_profile_bundle(const std::filesystem::path &path) {
  const auto contents = read_profile_file(path);
  if (!contents) return ProfileBundleLoadError::unreadable;
  auto parsed = layerc::core::parse_profile_json(*contents);
  auto *profile = std::get_if<layerc::core::GameProfile>(&parsed);
  if (profile == nullptr) return ProfileBundleLoadError::invalid_profile;

  ProfileBundleLoad result;
  result.profile = std::move(*profile);
  result.legacy_base_schema = source_schema_before(
      *contents, layerc::core::legacy_profile_schema_version);
  result.base_schema_below_items = source_schema_before(
      *contents, layerc::core::profile_schema_version);
  result.modules = load_profile_modules(path, result.profile);
  result.legacy_visual_schema = visual_sidecar_is_legacy(
      result.modules.get<core::VisualModule>());
  return result;
}

} // namespace layerc::platform
