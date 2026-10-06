#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/companion.hpp"
#include "layerc/core/profile_modules.hpp"
#include "layerc/core/types.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace layerc::platform {

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
// `<data>/<name>.json` (split stores) or `_<name>/<profile file>` (unified).
[[nodiscard]] std::filesystem::path
module_sidecar_path(const std::filesystem::path &profile_path,
                    const core::ModuleDescriptor &module);
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
sidecar_paths(const std::filesystem::path &profile_path,
              core::ProfileModuleKind kind);
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
  // Per-file problems; the scan itself still succeeded.
  std::vector<ProfileScanFault> faults;
  // Whole-scan failure: no profile list can be trusted.
  std::string error;

  // The fault messages joined with "; " for one-line diagnostics.
  [[nodiscard]] std::string warning_text() const;
  [[nodiscard]] const StoredProfile *find(std::string_view id) const noexcept;
  // Store path for `id` inside `directory`: the file that already holds the
  // id, else the first `<slug>.json`, `<slug>-2.json`, ... whose casefolded
  // name no scanned file uses, so distinct ids never share one file.
  [[nodiscard]] std::filesystem::path
  path_for(const std::filesystem::path &directory, std::string_view id) const;
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

// What a bundle save does with one module sidecar.
struct KeepModule {};   // retain the stored sidecar (invalid data blocks the save)
struct RemoveModule {}; // tombstone the sidecar

template <typename Profile> class ModuleIntent {
public:
  ModuleIntent(KeepModule) : value_(KeepModule{}) {}
  ModuleIntent(RemoveModule) : value_(RemoveModule{}) {}
  ModuleIntent(Profile replacement) : value_(std::move(replacement)) {}
  // An optional sidecar replaces when present and is removed when absent.
  ModuleIntent(const std::optional<Profile> &replacement)
      : value_(replacement.has_value()
                   ? std::variant<KeepModule, RemoveModule, Profile>(
                         *replacement)
                   : std::variant<KeepModule, RemoveModule, Profile>(
                         RemoveModule{})) {}
  ModuleIntent(std::nullopt_t) : value_(RemoveModule{}) {}

  [[nodiscard]] bool keeps() const noexcept {
    return std::holds_alternative<KeepModule>(value_);
  }
  [[nodiscard]] const Profile *replacement() const noexcept {
    return std::get_if<Profile>(&value_);
  }

private:
  std::variant<KeepModule, RemoveModule, Profile> value_;
};

template <typename Module>
using ModuleIntentFor = ModuleIntent<typename Module::Profile>;

// One intent per ProfileModuleTable entry, in table order.
using ProfileModuleIntents = core::ProfileModuleTable::each<ModuleIntentFor>;

// Commits a mutually compatible base profile and its first-party module
// sidecars. Optional module failure never disables base input/overlays.
[[nodiscard]] ProfileWriteResult
write_profile_bundle(const std::filesystem::path &path,
                     const layerc::core::GameProfile &profile,
                     ProfileModuleIntents modules);

// The store's bundle save policy, for planners that compose several bundles
// into one commit_profile_transaction: validates `profile` and its module
// intents against the stored bundle at `path`, then appends the base and
// sidecar recovery copies (the last usable primary or recovered backup, else
// the new contents), primaries, and markers or tombstones. `create_only`
// requires the base to be absent at commit. Call only while owning the
// store (inside a planner) unless the plan is advisory; nothing is appended
// on failure.
[[nodiscard]] ProfileWriteResult plan_profile_bundle_mutations(
    const std::filesystem::path &path, const layerc::core::GameProfile &profile,
    ProfileModuleIntents modules, bool create_only,
    std::vector<ProfileFileMutation> &mutations);

// Editors that own visual and automation but not companion data: the
// companion sidecar is kept unless a replacement is given.
[[nodiscard]] ProfileWriteResult write_profile_bundle(
    const std::filesystem::path &path, const layerc::core::GameProfile &profile,
    ModuleIntent<layerc::core::VisualProfile> visual,
    ModuleIntent<layerc::core::AutomationProfile> automation = RemoveModule{},
    ModuleIntent<layerc::core::CompanionProfile> companion = KeepModule{});

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

// One sidecar load outcome. Only the four legal states are constructible:
// a usable state always carries a profile, an invalid one always an error.
template <typename Profile, typename Error> class ModuleSidecarLoad {
public:
  ModuleSidecarLoad() = default;

  [[nodiscard]] static ModuleSidecarLoad
  absent(std::filesystem::path source_path) {
    return {Absent{}, std::move(source_path)};
  }
  [[nodiscard]] static ModuleSidecarLoad
  valid(Profile profile, std::filesystem::path source_path) {
    return {Valid{std::move(profile)}, std::move(source_path)};
  }
  // The backup was loaded because the primary failed with `primary_error`.
  [[nodiscard]] static ModuleSidecarLoad
  recovered(Profile profile, Error primary_error,
            std::filesystem::path source_path) {
    return {Recovered{std::move(profile), std::move(primary_error)},
            std::move(source_path)};
  }
  [[nodiscard]] static ModuleSidecarLoad
  invalid(Error error, std::filesystem::path source_path) {
    return {Invalid{std::move(error)}, std::move(source_path)};
  }

  [[nodiscard]] ModuleSidecarState state() const noexcept {
    return static_cast<ModuleSidecarState>(outcome_.index());
  }
  [[nodiscard]] bool usable() const noexcept { return profile() != nullptr; }
  // The loaded profile for valid and recovered sidecars.
  [[nodiscard]] const Profile *profile() const noexcept {
    if (const auto *valid = std::get_if<Valid>(&outcome_))
      return &valid->profile;
    if (const auto *recovered = std::get_if<Recovered>(&outcome_))
      return &recovered->profile;
    return nullptr;
  }
  [[nodiscard]] std::optional<Profile> optional_profile() const {
    if (const auto *loaded = profile()) return *loaded;
    return std::nullopt;
  }
  // Why the primary is unusable: invalid, or recovered from the backup.
  [[nodiscard]] const Error *error() const noexcept {
    if (const auto *invalid = std::get_if<Invalid>(&outcome_))
      return &invalid->error;
    if (const auto *recovered = std::get_if<Recovered>(&outcome_))
      return &recovered->primary_error;
    return nullptr;
  }
  [[nodiscard]] const std::filesystem::path &source_path() const noexcept {
    return source_path_;
  }

private:
  struct Absent {};
  struct Valid {
    Profile profile;
  };
  struct Recovered {
    Profile profile;
    Error primary_error;
  };
  struct Invalid {
    Error error;
  };
  // Alternative order matches ModuleSidecarState.
  using Outcome = std::variant<Absent, Valid, Recovered, Invalid>;

  ModuleSidecarLoad(Outcome outcome, std::filesystem::path source_path)
      : outcome_(std::move(outcome)), source_path_(std::move(source_path)) {}

  Outcome outcome_;
  std::filesystem::path source_path_;
};

// A value copy of one sidecar for hosts that edit or reconfigure from it:
// the profile (present only while usable or after an in-memory edit), the
// stored state it was loaded in, and that load's diagnostic.
template <typename Profile> struct ModuleSidecar {
  std::optional<Profile> profile;
  ModuleSidecarState state{ModuleSidecarState::absent};
  std::string diagnostic;

  template <typename Error>
  [[nodiscard]] static ModuleSidecar
  from(const ModuleSidecarLoad<Profile, Error> &load) {
    return {.profile = load.optional_profile(),
            .state = load.state(),
            .diagnostic = load.error() != nullptr ? load.error()->message
                                                  : std::string{}};
  }
  [[nodiscard]] bool usable() const noexcept {
    return is_usable(state) && profile.has_value();
  }
  // After an in-memory edit the sidecar is valid when present, else absent.
  void mark_edited() {
    state = profile.has_value() ? ModuleSidecarState::valid
                                : ModuleSidecarState::absent;
    diagnostic.clear();
  }

  bool operator==(const ModuleSidecar &) const = default;
};

template <typename Module>
using ModuleSidecarLoadFor =
    ModuleSidecarLoad<typename Module::Profile, typename Module::Error>;
using VisualSidecarLoad = ModuleSidecarLoadFor<layerc::core::VisualModule>;
using AutomationSidecarLoad =
    ModuleSidecarLoadFor<layerc::core::AutomationModule>;
using CompanionSidecarLoad = ModuleSidecarLoadFor<layerc::core::CompanionModule>;

enum class SidecarMarker { absent, active, removed, unreadable, invalid };

// The raw primary/backup/marker reads behind every module sidecar load.
struct SidecarFiles {
  std::filesystem::path primary;
  std::filesystem::path backup;
  std::filesystem::path marker;
  SidecarMarker marker_state{SidecarMarker::absent};
  FileReadResult primary_read;
  FileReadResult backup_read;
};

[[nodiscard]] SidecarFiles read_sidecar_files(const std::filesystem::path &primary,
                                              std::size_t max_bytes);

// Typed load with primary/backup/state recovery, shared by every module.
template <typename Module>
[[nodiscard]] ModuleSidecarLoadFor<Module>
load_sidecar_file(const std::filesystem::path &primary,
                  const layerc::core::GameProfile &base) {
  using Profile = typename Module::Profile;
  using Error = typename Module::Error;
  const auto message = [](const std::string_view detail) {
    return std::string(Module::descriptor.name) + " sidecar " +
           std::string(detail);
  };
  const auto files = read_sidecar_files(primary, Module::descriptor.max_bytes);
  using Load = ModuleSidecarLoadFor<Module>;
  switch (files.marker_state) {
  case SidecarMarker::unreadable:
    return Load::invalid({"$.state", message("state could not be read")},
                         files.marker);
  case SidecarMarker::invalid:
    return Load::invalid({"$.state", message("state is invalid")},
                         files.marker);
  case SidecarMarker::removed:
    return Load::absent(files.marker);
  case SidecarMarker::absent:
  case SidecarMarker::active:
    break;
  }
  Error primary_error{
      "$", message(files.primary_read.status == FileReadStatus::too_large
                       ? "exceeds its byte limit"
                       : "could not be read")};
  if (files.primary_read.has_value()) {
    auto parsed =
        Module::parse_json(layerc::core::as_text(*files.primary_read), base);
    if (auto *profile = std::get_if<Profile>(&parsed))
      return Load::valid(std::move(*profile), files.primary);
    primary_error = std::get<Error>(std::move(parsed));
  }
  if (files.backup_read.has_value()) {
    auto parsed =
        Module::parse_json(layerc::core::as_text(*files.backup_read), base);
    if (auto *profile = std::get_if<Profile>(&parsed))
      return Load::recovered(std::move(*profile), std::move(primary_error),
                             files.backup);
  }
  if (files.primary_read.status == FileReadStatus::missing &&
      files.backup_read.status == FileReadStatus::missing)
    return Load::absent(files.primary);
  return Load::invalid(std::move(primary_error), files.primary);
}

template <typename Module>
[[nodiscard]] ModuleSidecarLoadFor<Module>
load_module_sidecar(const std::filesystem::path &profile_path,
                    const layerc::core::GameProfile &base_profile) {
  return load_sidecar_file<Module>(
      module_sidecar_path(profile_path, Module::descriptor), base_profile);
}

[[nodiscard]] inline VisualSidecarLoad
load_visual_sidecar(const std::filesystem::path &profile_path,
                    const layerc::core::GameProfile &base_profile) {
  return load_module_sidecar<layerc::core::VisualModule>(profile_path,
                                                         base_profile);
}

[[nodiscard]] inline AutomationSidecarLoad
load_automation_sidecar(const std::filesystem::path &profile_path,
                        const layerc::core::GameProfile &base_profile) {
  return load_module_sidecar<layerc::core::AutomationModule>(profile_path,
                                                             base_profile);
}

[[nodiscard]] inline CompanionSidecarLoad
load_companion_sidecar(const std::filesystem::path &profile_path,
                       const layerc::core::GameProfile &base_profile) {
  return load_module_sidecar<layerc::core::CompanionModule>(profile_path,
                                                            base_profile);
}

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
struct CheckpointNotDue {};
struct CheckpointSaved {};
// One checkpoint opportunity: nothing was due, the state was saved, or the
// write failed (the deadline then slides out by one debounce for a retry).
using AutomationCheckpoint =
    std::variant<CheckpointNotDue, CheckpointSaved, ProfileWriteError>;

// The checkpoint policy every automation host shares: when `due` is ready
// (or `force`), write the state and report the outcome into `due`.
[[nodiscard]] AutomationCheckpoint checkpoint_automation_state(
    const std::filesystem::path &profile_path,
    const layerc::core::AutomationProfile &automation_profile,
    const layerc::core::AutomationPersistentValues &values,
    std::optional<layerc::core::TimePoint> &due, layerc::core::TimePoint now,
    bool force = false);

[[nodiscard]] bool signal_profile_reload();

} // namespace layerc::platform
