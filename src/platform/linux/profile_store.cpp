#include "layerc/platform/profile_store_linux.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/utf8.hpp"
#include "control_channel.hpp"

#include <cerrno>
#include <cstdlib>
#include <string_view>

#include <sys/stat.h>
#include <unistd.h>

namespace layerc::platform {
namespace {

std::filesystem::path environment_path(const char *const name) {
  const char *const value = std::getenv(name);
  return value == nullptr ? std::filesystem::path{}
                          : std::filesystem::path(value);
}

bool has_symlink_component(const std::filesystem::path &path) {
  std::filesystem::path current;
  for (const auto &part : path.lexically_normal()) {
    current /= part;
    struct stat status {};
    if (::lstat(current.c_str(), &status) == 0) {
      if (S_ISLNK(status.st_mode)) return true;
    } else if (errno != ENOENT) {
      return true;
    }
  }
  return false;
}

bool ensure_owned_directory(const std::filesystem::path &path) {
  if (path.empty() || !path.is_absolute() || has_symlink_component(path))
    return false;
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error || has_symlink_component(path)) return false;

  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != ::geteuid())
    return false;
  return ::chmod(path.c_str(), S_IRWXU) == 0;
}

} // namespace

ProfileStoreRoots profile_store_roots_from(
    const std::filesystem::path &home,
    const std::filesystem::path &config_home,
    const std::filesystem::path &data_home) {
  if (home.empty() || !home.is_absolute()) return {};
  const auto config = config_home.is_absolute() ? config_home
                                                 : home / ".config";
  const auto data = data_home.is_absolute() ? data_home
                                             : home / ".local" / "share";
  return {.config_profiles =
              (config / "layerc" / "profiles").lexically_normal(),
          .data_profiles =
              (data / "layerc" / "profiles").lexically_normal(),
          .store = (data / "layerc" / "store").lexically_normal()};
}

ProfileStoreRoots profile_store_roots() {
  return profile_store_roots_from(environment_path("HOME"),
                                  environment_path("XDG_CONFIG_HOME"),
                                  environment_path("XDG_DATA_HOME"));
}

bool ensure_profile_store_roots(const ProfileStoreRoots &roots) {
  if (roots.config_profiles.empty() || roots.data_profiles.empty() ||
      roots.store.empty())
    return false;
  return ensure_owned_directory(roots.config_profiles.parent_path()) &&
         ensure_owned_directory(roots.config_profiles) &&
         ensure_owned_directory(roots.data_profiles.parent_path()) &&
         ensure_owned_directory(roots.data_profiles) &&
         ensure_owned_directory(roots.store);
}

std::filesystem::path profile_directory() {
  return profile_store_roots().config_profiles;
}

std::filesystem::path visual_directory() {
  return profile_store_roots().data_profiles;
}

std::filesystem::path
profile_data_directory(const std::filesystem::path &profile_path) {
  const auto filename = profile_path.filename();
  if (filename.empty() || filename == "." || filename == "..") return {};
  return profile_store_roots().data_profiles / profile_path.stem();
}

std::filesystem::path profile_visual_asset_path(
    const std::filesystem::path &profile_path, const std::string_view sha256) {
  const auto data = profile_data_directory(profile_path);
  if (data.empty() || !layerc::core::is_lowercase_sha256(sha256)) return {};
  return data / "assets" / (std::string(sha256) + ".png");
}

std::filesystem::path
visual_sidecar_path(const std::filesystem::path &profile_path) {
  const auto data = profile_data_directory(profile_path);
  return data.empty() ? data : data / "visual.json";
}

std::filesystem::path
automation_sidecar_path(const std::filesystem::path &profile_path) {
  const auto data = profile_data_directory(profile_path);
  return data.empty() ? data : data / "automation.json";
}

std::filesystem::path
automation_state_path(const std::filesystem::path &profile_path) {
  const auto data = profile_data_directory(profile_path);
  return data.empty() ? data : data / "automation-state.json";
}

std::filesystem::path
companion_sidecar_path(const std::filesystem::path &profile_path) {
  const auto data = profile_data_directory(profile_path);
  return data.empty() ? data : data / "companion.json";
}

std::string canonical_profile_key(const std::filesystem::path &path) {
  if (path.empty()) return {};
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error);
  if (error) {
    error.clear();
    normalized = std::filesystem::absolute(path, error);
  }
  const auto bytes =
      (error ? path.lexically_normal() : normalized.lexically_normal())
          .generic_string();
  return layerc::core::try_casefold_utf8(bytes).value_or(std::string{});
}

bool signal_profile_reload() {
  std::string error;
  return control_roundtrip(
             encode_control_request(ControlCommand::notify_reload), error)
      .has_value();
}

} // namespace layerc::platform
