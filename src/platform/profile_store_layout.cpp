#include "layerc/platform/profile_store.hpp"
#include "profile_store_layout.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/utf8.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <string_view>

namespace layerc::platform {
namespace {

// One module file per profile: `<dir>/<profile filename><suffix>` beside a
// unified store, `<data dir>/<data_name>` in a split store.
std::filesystem::path
module_file_path(const std::filesystem::path &profile_path,
                 const std::string_view unified_directory,
                 const std::string_view unified_suffix,
                 const std::string_view data_name) {
  if (!profile_roots_unified_targets(profile_layout_roots(profile_path))) {
    const auto data = profile_data_directory(profile_path);
    return data.empty() ? data : data / data_name;
  }
  if (profile_path.filename().empty()) return {};
  auto filename = profile_path.filename();
  filename += unified_suffix;
  return profile_path.parent_path() / unified_directory / filename;
}

} // namespace

std::filesystem::path
profile_data_directory(const std::filesystem::path &profile_path) {
  const auto roots = profile_layout_roots(profile_path);
  if (profile_roots_unified_targets(roots)) return profile_path.parent_path();
  const auto filename = profile_path.filename();
  if (filename.empty() || filename == "." || filename == "..") return {};
  return roots.data_profiles / profile_path.stem();
}

std::filesystem::path
profile_visual_asset_directory(const std::filesystem::path &profile_path) {
  if (profile_path.empty()) return {};
  const auto roots = profile_layout_roots(profile_path);
  if (profile_roots_unified_targets(roots))
    return unified_visual_asset_directory(profile_path.parent_path());
  const auto data = profile_data_directory(profile_path);
  return data.empty() ? data : data / "assets";
}

std::filesystem::path
unified_visual_asset_directory(const std::filesystem::path &profile_directory) {
  return profile_directory / "_visual" / "assets";
}

std::filesystem::path visual_asset_file(const std::filesystem::path &directory,
                                        const std::string_view sha256) {
  if (directory.empty() || !layerc::core::is_lowercase_sha256(sha256))
    return {};
  return directory / (std::string(sha256) + ".png");
}

std::filesystem::path profile_visual_asset_path(
    const std::filesystem::path &profile_path, const std::string_view sha256) {
  return visual_asset_file(profile_visual_asset_directory(profile_path),
                           sha256);
}

std::filesystem::path
module_sidecar_path(const std::filesystem::path &profile_path,
                    const core::ModuleDescriptor &module) {
  const std::string name(module.name);
  return module_file_path(profile_path, "_" + name, "", name + ".json");
}

std::filesystem::path
visual_sidecar_path(const std::filesystem::path &profile_path) {
  return module_sidecar_path(profile_path, core::VisualModule::descriptor);
}

std::filesystem::path
automation_sidecar_path(const std::filesystem::path &profile_path) {
  return module_sidecar_path(profile_path, core::AutomationModule::descriptor);
}

std::filesystem::path
automation_state_path(const std::filesystem::path &profile_path) {
  return module_file_path(profile_path, "_state", ".automation-state.json",
                          "automation-state.json");
}

std::filesystem::path
companion_sidecar_path(const std::filesystem::path &profile_path) {
  return module_sidecar_path(profile_path, core::CompanionModule::descriptor);
}

std::filesystem::path profile_store_filename(const std::string_view profile_id) {
  return std::filesystem::path(core::slugify(profile_id, "profile") + ".json");
}

std::string canonical_profile_key(const std::filesystem::path &path) {
  if (path.empty()) return {};
  auto normalized = profile_layout_final_path(path);
  if (normalized.empty()) {
    std::error_code error;
    normalized = std::filesystem::weakly_canonical(path, error);
    if (error) {
      error.clear();
      normalized = std::filesystem::absolute(path, error);
    }
    if (error) normalized = path;
  }
  auto encoded = path_to_utf8(normalized.lexically_normal());
  if (!encoded.has_value()) return {};
  // Native separators: keys keep the platform's spelling of the path.
  std::ranges::replace(
      *encoded, '/',
      static_cast<char>(std::filesystem::path::preferred_separator));
  return layerc::core::try_casefold_utf8(*encoded).value_or(std::string{});
}

} // namespace layerc::platform
