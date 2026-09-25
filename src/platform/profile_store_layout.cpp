#include "layerc/platform/profile_store.hpp"
#include "profile_store_layout.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/utf8.hpp"

#include <algorithm>
#include <string_view>

namespace layerc::platform {
namespace {

std::filesystem::path profile_data_file(const std::filesystem::path &profile_path,
                                        const std::string_view name) {
  const auto data = profile_data_directory(profile_path);
  return data.empty() ? data : data / name;
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
    return profile_path.parent_path() / "_visual" / "assets";
  const auto data = profile_data_directory(profile_path);
  return data.empty() ? data : data / "assets";
}

std::filesystem::path profile_visual_asset_path(
    const std::filesystem::path &profile_path, const std::string_view sha256) {
  if (!layerc::core::is_lowercase_sha256(sha256)) return {};
  const auto directory = profile_visual_asset_directory(profile_path);
  return directory.empty() ? directory
                           : directory / (std::string(sha256) + ".png");
}

std::filesystem::path
visual_sidecar_path(const std::filesystem::path &profile_path) {
  const auto roots = profile_layout_roots(profile_path);
  if (profile_roots_unified_targets(roots)) {
    if (profile_path.filename().empty()) return {};
    return profile_path.parent_path() / "_visual" / profile_path.filename();
  }
  return profile_data_file(profile_path, "visual.json");
}

std::filesystem::path
automation_sidecar_path(const std::filesystem::path &profile_path) {
  const auto roots = profile_layout_roots(profile_path);
  if (profile_roots_unified_targets(roots)) {
    if (profile_path.filename().empty()) return {};
    return profile_path.parent_path() / "_automation" / profile_path.filename();
  }
  return profile_data_file(profile_path, "automation.json");
}

std::filesystem::path
automation_state_path(const std::filesystem::path &profile_path) {
  const auto roots = profile_layout_roots(profile_path);
  if (profile_roots_unified_targets(roots)) {
    if (profile_path.filename().empty()) return {};
    auto filename = profile_path.filename();
    filename += ".automation-state.json";
    return profile_path.parent_path() / "_state" / filename;
  }
  return profile_data_file(profile_path, "automation-state.json");
}

std::filesystem::path
companion_sidecar_path(const std::filesystem::path &profile_path) {
  const auto roots = profile_layout_roots(profile_path);
  if (profile_roots_unified_targets(roots)) {
    if (profile_path.filename().empty()) return {};
    return profile_path.parent_path() / "_companion" / profile_path.filename();
  }
  return profile_data_file(profile_path, "companion.json");
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
  normalized = normalized.lexically_normal();
#ifdef _WIN32
  auto wide = normalized.wstring();
  std::replace(wide.begin(), wide.end(), L'/', L'\\');
  const auto encoded = layerc::core::try_wide_to_utf8(wide);
  if (!encoded.has_value()) return {};
  return layerc::core::try_casefold_utf8(*encoded).value_or(std::string{});
#else
  return layerc::core::try_casefold_utf8(normalized.generic_string())
      .value_or(std::string{});
#endif
}

} // namespace layerc::platform
