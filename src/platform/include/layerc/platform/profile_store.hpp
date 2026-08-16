#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/visual.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::platform {

inline constexpr wchar_t profile_reload_event_name[] =
    L"Local\\LayerC.LayerD.ProfileReload";
inline constexpr wchar_t automation_active_event_name[] =
    L"Local\\LayerC.LayerD.AutomationActive.v1";

[[nodiscard]] std::filesystem::path profile_directory();
[[nodiscard]] std::filesystem::path visual_directory();
[[nodiscard]] std::filesystem::path automation_directory();
[[nodiscard]] std::filesystem::path automation_state_directory();
[[nodiscard]] std::filesystem::path
visual_sidecar_path(const std::filesystem::path &profile_path);
[[nodiscard]] std::filesystem::path
automation_sidecar_path(const std::filesystem::path &profile_path);
[[nodiscard]] std::filesystem::path
automation_state_path(const std::filesystem::path &profile_path);
[[nodiscard]] std::wstring
canonical_profile_key(const std::filesystem::path &path);
[[nodiscard]] std::optional<std::string>
read_profile_file(const std::filesystem::path &path);
[[nodiscard]] std::optional<std::string>
read_visual_file(const std::filesystem::path &path);
[[nodiscard]] std::optional<std::string>
read_automation_file(const std::filesystem::path &path);
[[nodiscard]] bool write_profile_file(const std::filesystem::path &path,
                                      std::string_view contents);

// Commits a mutually compatible base profile and its declared first-party
// module sidecars. Optional module failure never disables base input/overlays.
[[nodiscard]] bool write_profile_bundle(
    const std::filesystem::path &path, const layerc::core::GameProfile &profile,
    const std::optional<layerc::core::VisualProfile> &visual_profile,
    const std::optional<layerc::core::AutomationProfile> &automation_profile =
        std::nullopt);

enum class VisualSidecarState {
  absent,
  valid,
  recovered_backup,
  invalid,
};

struct VisualSidecarLoad {
  VisualSidecarState state{VisualSidecarState::absent};
  std::optional<layerc::core::VisualProfile> profile;
  layerc::core::VisualError error;
  std::filesystem::path source_path;
};

[[nodiscard]] VisualSidecarLoad
load_visual_sidecar(const std::filesystem::path &profile_path,
                    const layerc::core::GameProfile &base_profile);
[[nodiscard]] bool
write_visual_sidecar(const std::filesystem::path &profile_path,
                     const layerc::core::VisualProfile &visual_profile,
                     const layerc::core::GameProfile &base_profile);

enum class AutomationSidecarState {
  absent,
  valid,
  recovered_backup,
  invalid,
};

struct AutomationSidecarLoad {
  AutomationSidecarState state{AutomationSidecarState::absent};
  std::optional<layerc::core::AutomationProfile> profile;
  layerc::core::AutomationError error;
  std::filesystem::path source_path;
};

[[nodiscard]] AutomationSidecarLoad
load_automation_sidecar(const std::filesystem::path &profile_path,
                        const layerc::core::GameProfile &base_profile);
[[nodiscard]] bool write_automation_sidecar(
    const std::filesystem::path &profile_path,
    const layerc::core::AutomationProfile &automation_profile,
    const layerc::core::GameProfile &base_profile);

struct AutomationStateLoad {
  layerc::core::AutomationPersistentValues values;
  std::optional<layerc::core::AutomationError> error;
};

[[nodiscard]] AutomationStateLoad load_automation_state(
    const std::filesystem::path &profile_path,
    const layerc::core::AutomationProfile &automation_profile);
[[nodiscard]] bool write_automation_state(
    const std::filesystem::path &profile_path,
    const layerc::core::AutomationProfile &automation_profile,
    const layerc::core::AutomationPersistentValues &values);

[[nodiscard]] bool signal_profile_reload();

} // namespace layerc::platform