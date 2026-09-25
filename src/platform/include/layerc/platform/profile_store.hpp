#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/companion.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/profile_transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace layerc::platform {

enum class ModuleSidecarKind : std::uint8_t {
  visual,
  automation,
  companion,
};

struct SidecarPaths {
  std::filesystem::path primary;
  std::filesystem::path backup;
  std::filesystem::path marker;
};

struct ProfileStoreRoots {
  std::filesystem::path config_profiles;
  std::filesystem::path data_profiles;
  std::filesystem::path store;
};

// Typed roots topology behind the transaction engine. Unified stores
// (Windows: config == data == store) nest the transaction journal below
// both target roots, so the file guard reserves its directory name.
// Split stores (Linux) keep the journal outside both target roots and
// reserve nothing. Branch on this — never on the platform — when topology
// is what actually differs.
[[nodiscard]] inline bool profile_roots_unified_targets(
    const ProfileStoreRoots &roots) {
  return roots.config_profiles.lexically_normal() ==
         roots.data_profiles.lexically_normal();
}


[[nodiscard]] std::filesystem::path profile_directory();
[[nodiscard]] std::filesystem::path
profile_data_directory(const std::filesystem::path &profile_path);
[[nodiscard]] std::filesystem::path
profile_visual_asset_directory(const std::filesystem::path &profile_path);
// The asset directory every profile of a unified store directory shares.
[[nodiscard]] std::filesystem::path
unified_visual_asset_directory(const std::filesystem::path &profile_directory);
// `<directory>/<sha256>.png`, or empty for an empty directory or invalid hash.
[[nodiscard]] std::filesystem::path
visual_asset_file(const std::filesystem::path &directory,
                  std::string_view sha256);
[[nodiscard]] std::filesystem::path profile_visual_asset_path(
    const std::filesystem::path &profile_path, std::string_view sha256);
[[nodiscard]] std::filesystem::path
visual_sidecar_path(const std::filesystem::path &profile_path);
[[nodiscard]] std::filesystem::path
automation_sidecar_path(const std::filesystem::path &profile_path);
[[nodiscard]] std::filesystem::path
automation_state_path(const std::filesystem::path &profile_path);
[[nodiscard]] std::filesystem::path
companion_sidecar_path(const std::filesystem::path &profile_path);
[[nodiscard]] std::filesystem::path
recovery_backup_path(const std::filesystem::path &path);
[[nodiscard]] SidecarPaths
sidecar_paths(const std::filesystem::path &profile_path, ModuleSidecarKind kind);
[[nodiscard]] std::string_view sidecar_marker_contents(bool active) noexcept;
[[nodiscard]] std::string
canonical_profile_key(const std::filesystem::path &path);
// Store filename for a new profile: lower-case ASCII letters and digits, one
// dash for every other run, "profile" when nothing remains, plus ".json".
// Profile ids never become path components directly.
[[nodiscard]] std::filesystem::path
profile_store_filename(std::string_view profile_id);
[[nodiscard]] std::optional<std::string>
read_profile_file(const std::filesystem::path &path);

inline constexpr std::size_t max_profile_directory_entries = 1024U;

struct StoredProfile {
  std::filesystem::path path;
  layerc::core::GameProfile profile;
};

struct ProfileScanFault {
  std::filesystem::path path;
  std::string message;
};

struct ProfileDirectoryScan {
  std::vector<StoredProfile> profiles;
  // Every regular top-level JSON path, including files that could not be read
  // or parsed. Import planning reserves all of their names.
  std::vector<std::filesystem::path> base_paths;
  std::vector<ProfileScanFault> faults;
  std::string warning;
  std::string error;

  [[nodiscard]] const StoredProfile *find(std::string_view id) const noexcept;
};

[[nodiscard]] bool has_json_extension(const std::filesystem::path &path);
[[nodiscard]] ProfileDirectoryScan
scan_profile_directory(const std::filesystem::path &directory);

// Publishes one base-profile file through the stage→commit journal. A
// user-visible `.bak` of the previous parseable profile is published as a
// mutation of the same transaction (never promoted from invalid external
// edits). Visual assets stay content-addressed CAS instead: identity is the
// content hash and overwrites cannot occur.
[[nodiscard]] ProfileWriteResult write_profile_file(const std::filesystem::path &path,
                                      std::string_view contents);

// Commits a mutually compatible base profile and its declared first-party
// module sidecars. Optional module failure never disables base input/overlays.
[[nodiscard]] ProfileWriteResult write_profile_bundle(
    const std::filesystem::path &path, const layerc::core::GameProfile &profile,
    const std::optional<layerc::core::VisualProfile> &visual_profile,
    const std::optional<layerc::core::AutomationProfile> &automation_profile =
        std::nullopt,
    const std::optional<layerc::core::CompanionProfile> &companion_profile =
        std::nullopt);

// Like write_profile_bundle for a base profile without module sidecars, but
// fails atomically if the base profile already exists when the transaction
// lock is acquired.
[[nodiscard]] ProfileWriteResult
write_profile_bundle_create_only(const std::filesystem::path &path,
                                 const layerc::core::GameProfile &profile);

enum class ModuleSidecarState {
  absent,
  valid,
  recovered_backup,
  invalid,
};

[[nodiscard]] constexpr bool is_usable(const ModuleSidecarState state) noexcept {
  return state == ModuleSidecarState::valid ||
         state == ModuleSidecarState::recovered_backup;
}

// Recovered and invalid sidecars must be repaired explicitly before another
// module may rewrite the bundle, otherwise the recovery evidence is lost.
[[nodiscard]] constexpr bool
may_write_primary(const ModuleSidecarState state) noexcept {
  return state == ModuleSidecarState::absent ||
         state == ModuleSidecarState::valid;
}

template <typename Profile, typename Error> struct ModuleSidecarLoad {
  ModuleSidecarState state{ModuleSidecarState::absent};
  std::optional<Profile> profile;
  Error error;
  std::filesystem::path source_path;

  [[nodiscard]] bool usable() const noexcept {
    return is_usable(state) && profile.has_value();
  }
};

using VisualSidecarLoad =
    ModuleSidecarLoad<layerc::core::VisualProfile, layerc::core::VisualError>;
using AutomationSidecarLoad = ModuleSidecarLoad<
    layerc::core::AutomationProfile, layerc::core::AutomationError>;
using CompanionSidecarLoad = ModuleSidecarLoad<
    layerc::core::CompanionProfile, layerc::core::CompanionError>;

[[nodiscard]] VisualSidecarLoad
load_visual_sidecar(const std::filesystem::path &profile_path,
                    const layerc::core::GameProfile &base_profile);

[[nodiscard]] AutomationSidecarLoad
load_automation_sidecar(const std::filesystem::path &profile_path,
                        const layerc::core::GameProfile &base_profile);

[[nodiscard]] CompanionSidecarLoad
load_companion_sidecar(const std::filesystem::path &profile_path,
                       const layerc::core::GameProfile &base_profile);

struct AutomationStateLoad {
  layerc::core::AutomationPersistentValues values;
  std::optional<layerc::core::AutomationError> error;
};

[[nodiscard]] AutomationStateLoad load_automation_state(
    const std::filesystem::path &profile_path,
    const layerc::core::AutomationProfile &automation_profile);
// Publishes machine-local automation state through the stage→commit journal
// (one mutation, no user-visible `.bak`).
[[nodiscard]] ProfileWriteResult write_automation_state(
    const std::filesystem::path &profile_path,
    const layerc::core::AutomationProfile &automation_profile,
    const layerc::core::AutomationPersistentValues &values);
// The checkpoint policy every automation host shares: when `due` is ready
// (or `force`), write the state and report the outcome into `due`. Returns
// the write result, or nullopt when no checkpoint was due.
[[nodiscard]] std::optional<ProfileWriteResult> checkpoint_automation_state(
    const std::filesystem::path &profile_path,
    const layerc::core::AutomationProfile &automation_profile,
    const layerc::core::AutomationPersistentValues &values,
    std::optional<layerc::core::TimePoint> &due, layerc::core::TimePoint now,
    bool force = false);

struct SidecarMarkerInspection {
  bool present{};
  std::error_code error;
};

[[nodiscard]] SidecarMarkerInspection
inspect_sidecar_markers(const std::filesystem::path &sidecar) noexcept;

[[nodiscard]] bool signal_profile_reload();

} // namespace layerc::platform
