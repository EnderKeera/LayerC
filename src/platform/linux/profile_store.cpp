#include "layerc/platform/profile_store_linux.hpp"
#include "layerc/platform/profile_file_guard_linux.hpp"
#include "../profile_store_layout.hpp"
#include "control_channel.hpp"

#include <cstdlib>

namespace layerc::platform {
namespace {

std::filesystem::path environment_path(const char *const name) {
  const char *const value = std::getenv(name);
  return value == nullptr ? std::filesystem::path{}
                          : std::filesystem::path(value);
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

ProfileStoreRoots profile_layout_roots(const std::filesystem::path &) {
  return profile_store_roots();
}

std::filesystem::path profile_layout_final_path(const std::filesystem::path &) {
  return {};
}

bool signal_profile_reload() {
  std::string error;
  return control_roundtrip(
             encode_control_request(ControlCommand::notify_reload), error)
      .has_value();
}

} // namespace layerc::platform
