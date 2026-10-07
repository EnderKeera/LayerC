#include "layerc/platform/profile_store.hpp"

#include "layerc/core/automation_session.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/utf8.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_file_guard.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <unordered_set>
#include <utility>
#include <vector>

namespace layerc::platform {
namespace {

using layerc::core::as_text;

std::filesystem::path sidecar_state_path(const std::filesystem::path &sidecar) {
  auto state = sidecar;
  state += ".state";
  return state;
}

[[nodiscard]] ProfileFileMutation
text_mutation(const std::filesystem::path &target,
              const std::string_view contents) {
  return ProfileFileMutation{.target = target,
                             .contents = layerc::core::owned_bytes(contents)};
}

} // namespace

std::optional<std::string>
read_profile_file(const std::filesystem::path &path) {
  return read_durable_text(path, layerc::core::max_profile_json_bytes);
}

// --- Profile directory scan ---

std::string ProfileDirectoryScan::warning_text() const {
  std::string text;
  for (const auto &fault : faults) {
    if (!text.empty()) text += "; ";
    text += fault.message;
  }
  return text;
}

const StoredProfile *ProfileDirectoryScan::find(
    const std::string_view id) const noexcept {
  const auto found = std::find_if(
      profiles.begin(), profiles.end(), [id](const StoredProfile &entry) {
        return entry.profile.id == id;
      });
  return found == profiles.end() ? nullptr : &*found;
}

std::filesystem::path
ProfileDirectoryScan::path_for(const std::filesystem::path &directory,
                               const std::string_view id) const {
  if (const auto *stored = find(id)) return stored->path;
  const auto folded_name = [](const std::filesystem::path &path) {
    return core::try_casefold_utf8(
        path_to_utf8(path.filename()).value_or(std::string{}));
  };
  const auto taken = [&](const std::filesystem::path &name) {
    const auto folded = folded_name(name);
    return std::ranges::any_of(base_paths, [&](const auto &path) {
      return folded_name(path) == folded;
    });
  };
  const auto slug = core::slugify(id, "profile");
  auto name = std::filesystem::path(slug + ".json");
  // base_paths is bounded by max_profile_directory_entries, so this ends.
  for (std::size_t suffix = 2; taken(name); ++suffix)
    name = std::filesystem::path(slug + "-" + std::to_string(suffix) + ".json");
  return directory / name;
}

bool has_json_extension(const std::filesystem::path &path) {
  const auto encoded = path_to_utf8(path.extension());
  if (!encoded.has_value()) return false;
  const auto folded = core::try_casefold_utf8(*encoded);
  return folded.has_value() && *folded == ".json";
}

ProfileDirectoryScan
scan_profile_directory(const std::filesystem::path &directory) {
  ProfileDirectoryScan result;
  if (directory.empty()) {
    result.error = "profile directory is unavailable";
    return result;
  }

  std::error_code error;
  const bool exists = std::filesystem::exists(directory, error);
  if (error) {
    result.error = "profile directory could not be inspected";
    return result;
  }
  if (!exists) return result;

  const auto append_warning = [&result](const std::filesystem::path &path,
                                        std::string message) {
    result.faults.push_back({.path = path, .message = std::move(message)});
  };
  const auto filename = [](const std::filesystem::path &path) {
    return path_to_utf8(path.filename()).value_or("<invalid filename>");
  };

  std::unordered_set<std::string> ids;
  std::filesystem::directory_iterator iterator(directory, error), end;
  std::size_t entry_count{};
  while (!error && iterator != end) {
    if (++entry_count > max_profile_directory_entries) {
      result.error = "profile directory contains too many entries";
      return result;
    }
    const auto status = iterator->symlink_status(error);
    if (error) break;
    const auto path = iterator->path();
    if (std::filesystem::is_regular_file(status) && has_json_extension(path)) {
      result.base_paths.push_back(path);
      const auto contents = read_profile_file(path);
      if (!contents.has_value()) {
        append_warning(path, filename(path) + ": could not be read");
      } else {
        auto parsed = layerc::core::parse_profile_json(*contents);
        if (auto *profile =
                std::get_if<layerc::core::GameProfile>(&parsed)) {
          if (!ids.insert(profile->id).second) {
            result.error = "duplicate profile id '" + profile->id + "'";
            return result;
          }
          result.profiles.push_back(
              {.path = std::move(path), .profile = std::move(*profile)});
        } else {
          const auto &failure =
              std::get<layerc::core::ProfileError>(parsed);
          append_warning(path, filename(path) + ": " + failure.message);
        }
      }
    }
    iterator.increment(error);
  }
  if (error) result.error = "profile directory enumeration failed";
  return result;
}

// --- Sidecar paths and marker policy ---

std::filesystem::path
recovery_backup_path(const std::filesystem::path &path) {
  auto backup = path;
  backup += ".bak";
  return backup;
}

SidecarPaths sidecar_paths(const std::filesystem::path &profile_path,
                           const core::ProfileModuleKind kind) {
  const auto *module = core::find_module(kind);
  auto primary = module == nullptr
                     ? std::filesystem::path{}
                     : module_sidecar_path(profile_path, *module);
  auto marker = sidecar_state_path(primary);
  return {.primary = primary,
          .backup = recovery_backup_path(primary),
          .marker = std::move(marker)};
}

std::string_view sidecar_marker_contents(const bool active) noexcept {
  return active ? "active\n" : "removed\n";
}

// --- Sidecar files: the raw reads behind load_sidecar_file ---

SidecarFiles read_sidecar_files(const std::filesystem::path &primary,
                                const std::size_t max_bytes) {
  constexpr std::size_t max_sidecar_state_bytes = 32;
  SidecarFiles files;
  files.primary = primary;
  files.backup = recovery_backup_path(primary);
  files.marker = sidecar_state_path(primary);
  auto marker = read_durable_file(files.marker, max_sidecar_state_bytes);
  if (marker.status != FileReadStatus::missing) {
    if (!marker.has_value()) {
      files.marker_state = SidecarMarker::unreadable;
      return files;
    }
    const auto contents = as_text(*marker);
    if (contents == sidecar_marker_contents(false)) {
      files.marker_state = SidecarMarker::removed;
      return files;
    }
    if (contents != sidecar_marker_contents(true)) {
      files.marker_state = SidecarMarker::invalid;
      return files;
    }
    files.marker_state = SidecarMarker::active;
  }
  files.primary_read = read_durable_file(files.primary, max_bytes);
  files.backup_read = read_durable_file(files.backup, max_bytes);
  return files;
}

// Single-file durable writers. Both publish through commit_profile_transaction
// so profile/state mutations share one journal, one-writer lock, and crash
// recovery with bundle saves. Visual assets stay content-addressed CAS
// (hash-named immutable PNGs via write_asset_atomically): identity is the
// content hash, overwrites cannot occur, and there is no user-visible `.bak`.
ProfileWriteResult write_profile_file(const std::filesystem::path &path,
                                      const std::string_view contents) {
  if (contents.size() > layerc::core::max_profile_json_bytes)
    return {ProfileWriteError::invalid_input};
  std::error_code path_error;
  const auto absolute = std::filesystem::absolute(path, path_error);
  if (path_error) return {ProfileWriteError::invalid_input};

  return commit_profile_transaction(
      absolute.parent_path(), [&](std::vector<ProfileFileMutation> &mutations)
                                  -> ProfileWriteResult {
        // Read the immediately preceding recovered profile under ownership.
        // Invalid external edits are never promoted into the recovery backup.
        const auto previous = read_profile_file(absolute);
        if (previous.has_value() &&
            std::holds_alternative<layerc::core::GameProfile>(
                layerc::core::parse_profile_json(*previous)))
          mutations.push_back(
              text_mutation(recovery_backup_path(absolute), *previous));
        mutations.push_back(text_mutation(absolute, contents));
        return {};
      });
}

// --- Bundle planning and publication share recovered writer ownership ---
namespace {

struct BundleInputs {
  std::filesystem::path path;
  layerc::core::GameProfile profile;
  ProfileModuleIntents modules;
  bool create_only{};
};

[[nodiscard]] std::optional<bool>
path_exists(const std::filesystem::path &target) {
  std::error_code error;
  const bool found = std::filesystem::exists(target, error);
  if (error) return std::nullopt;
  return found;
}

// One module's part of a bundle plan: resolves the intent against the stored
// sidecar, validates it against the new base, declares it in `stored`, and
// appends its recovery copy, primary, and marker (or tombstone) mutations.
template <typename Module>
[[nodiscard]] bool plan_module_sidecar(
    const ModuleIntentFor<Module> &intent, const std::filesystem::path &path,
    const std::optional<layerc::core::GameProfile> &previous_profile,
    layerc::core::GameProfile &stored,
    std::vector<ProfileFileMutation> &mutations, ProfileWriteError &failure) {
  std::optional<typename Module::Profile> effective;
  if (const auto *replacement = intent.replacement()) {
    effective = *replacement;
  } else if (intent.keeps()) {
    // Keeping is owned by the bundle: invalid stored data blocks the save
    // instead of being silently dropped or overwritten.
    const auto existing = load_module_sidecar<Module>(
        path, previous_profile.has_value() ? *previous_profile : stored);
    if (existing.state() == ModuleSidecarState::invalid) return false;
    effective = existing.optional_profile();
  }
  if (effective.has_value()) {
    if (!Module::valid(*effective, stored)) return false;
    stored.modules.push_back({.kind = Module::descriptor.kind,
                              .schema_version = effective->schema_version});
  }

  const auto paths = sidecar_paths(path, Module::descriptor.kind);
  if (paths.primary.empty()) return false;
  const auto primary_exists = path_exists(paths.primary);
  const auto backup_exists = path_exists(paths.backup);
  const auto marker_exists = path_exists(paths.marker);
  if (!primary_exists || !backup_exists || !marker_exists) {
    failure = ProfileWriteError::io_failed;
    return false;
  }
  if (!effective.has_value()) {
    if (*primary_exists || *backup_exists || *marker_exists)
      mutations.push_back(text_mutation(
          paths.marker, std::string(sidecar_marker_contents(false))));
    return true;
  }

  // Prefer the last usable sidecar bytes as the user-visible recovery copy;
  // fall back to the new contents when nothing usable was stored.
  const auto contents = Module::serialize(*effective);
  std::string recovery = contents;
  if (previous_profile.has_value()) {
    const auto loaded =
        load_sidecar_file<Module>(paths.primary, *previous_profile);
    if (loaded.usable()) {
      auto source =
          read_durable_file(loaded.source_path(), Module::descriptor.max_bytes);
      if (!source.has_value()) return false;
      recovery = as_text(*source);
    }
  }
  mutations.push_back(text_mutation(paths.backup, recovery));
  mutations.push_back(text_mutation(paths.primary, contents));
  mutations.push_back(
      text_mutation(paths.marker, std::string(sidecar_marker_contents(true))));
  return true;
}

// Validate + assemble under recovered writer ownership. Nullopt when the
// inputs cannot become a committable bundle.
[[nodiscard]] std::optional<std::vector<ProfileFileMutation>>
plan_profile_bundle(BundleInputs inputs, ProfileWriteError &failure) {
  auto stored = std::move(inputs.profile);
  stored.schema_version = layerc::core::profile_schema_version;
  stored.modules.clear();
  const auto &path = inputs.path;
  if (path.parent_path().empty()) return std::nullopt;

  const auto previous_json = read_profile_file(path);
  std::optional<layerc::core::GameProfile> previous_profile;
  if (previous_json.has_value()) {
    auto parsed = layerc::core::parse_profile_json(*previous_json);
    if (auto *profile = std::get_if<layerc::core::GameProfile>(&parsed))
      previous_profile = std::move(*profile);
  }

  std::vector<ProfileFileMutation> mutations;
  // Preserve the last parseable base as the user-visible recovery copy. The
  // transaction journal itself separately owns rollback snapshots.
  if (previous_profile.has_value())
    mutations.push_back(
        text_mutation(recovery_backup_path(path), *previous_json));

  // Recovery copies are published first. If one is locked or unwritable, the
  // journal rolls back before the new base can become authoritative.
  bool planned = true;
  layerc::core::ProfileModuleTable::for_each([&]<typename Module>(Module) {
    planned = planned &&
              plan_module_sidecar<Module>(
                  std::get<ModuleIntentFor<Module>>(inputs.modules), path,
                  previous_profile, stored, mutations, failure);
  });
  if (!planned) return std::nullopt;

  mutations.push_back(
      text_mutation(path, layerc::core::serialize_profile_json(stored)));
  mutations.back().must_be_absent = inputs.create_only;
  return mutations;
}

// The durable tombstone is authoritative. Retain removed sidecar bytes:
// deleting after the transaction releases writer ownership could remove a
// concurrent writer's newly activated sidecar and recovery copy.
ProfileWriteResult write_profile_bundle_impl(BundleInputs inputs) {
  const auto directory = inputs.path.parent_path();
  return commit_profile_transaction(
      directory, [&](std::vector<ProfileFileMutation> &mutations)
                     -> ProfileWriteResult {
        return plan_profile_bundle_mutations(
            inputs.path, inputs.profile, std::move(inputs.modules),
            inputs.create_only, mutations);
      });
}

} // namespace

ProfileWriteResult plan_profile_bundle_mutations(
    const std::filesystem::path &path, const layerc::core::GameProfile &profile,
    ProfileModuleIntents modules, const bool create_only,
    std::vector<ProfileFileMutation> &mutations) {
  ProfileWriteError failure = ProfileWriteError::invalid_input;
  auto plan = plan_profile_bundle({.path = path,
                                   .profile = profile,
                                   .modules = std::move(modules),
                                   .create_only = create_only},
                                  failure);
  if (!plan.has_value()) return {failure};
  mutations.insert(mutations.end(), std::make_move_iterator(plan->begin()),
                   std::make_move_iterator(plan->end()));
  return {};
}

ProfileWriteResult write_profile_bundle(const std::filesystem::path &path,
                                        const layerc::core::GameProfile &profile,
                                        ProfileModuleIntents modules) {
  return write_profile_bundle_impl({.path = path,
                                    .profile = profile,
                                    .modules = std::move(modules),
                                    .create_only = false});
}

ProfileWriteResult write_profile_bundle(
    const std::filesystem::path &path, const layerc::core::GameProfile &profile,
    ModuleIntent<layerc::core::VisualProfile> visual,
    ModuleIntent<layerc::core::AutomationProfile> automation,
    ModuleIntent<layerc::core::CompanionProfile> companion) {
  // The editor owns these modules only; every other registered module keeps
  // its stored state without changing this convenience API.
  ProfileModuleIntents modules;
  std::get<ModuleIntentFor<core::VisualModule>>(modules) = std::move(visual);
  std::get<ModuleIntentFor<core::AutomationModule>>(modules) = std::move(automation);
  std::get<ModuleIntentFor<core::CompanionModule>>(modules) = std::move(companion);
  return write_profile_bundle(path, profile, std::move(modules));
}

ProfileWriteResult
write_profile_bundle_create_only(const std::filesystem::path &path,
                                 const layerc::core::GameProfile &profile) {
  ProfileModuleIntents modules;
  std::get<ModuleIntentFor<core::VisualModule>>(modules) = RemoveModule{};
  std::get<ModuleIntentFor<core::AutomationModule>>(modules) = RemoveModule{};
  return write_profile_bundle_impl(
      {.path = path,
       .profile = profile,
       .modules = std::move(modules),
       .create_only = true});
}

AutomationStateLoad load_automation_state(
    const std::filesystem::path &profile_path,
    const layerc::core::AutomationProfile &automation_profile) {
  const auto path = automation_state_path(profile_path);
  const auto contents =
      read_durable_file(path, layerc::core::max_automation_state_bytes);
  if (contents.status == FileReadStatus::missing) return {};
  if (!contents.has_value())
    return {.values = {},
            .error = layerc::core::AutomationError{
                "$", contents.status == FileReadStatus::too_large
                         ? "automation state exceeds its byte limit"
                         : "automation state could not be read"}};
  auto parsed =
      layerc::core::parse_automation_state_json(as_text(*contents),
                                                automation_profile);
  if (auto *values =
          std::get_if<layerc::core::AutomationPersistentValues>(&parsed))
    return {.values = std::move(*values), .error = std::nullopt};
  return {.values = {},
          .error = std::get<layerc::core::AutomationError>(std::move(parsed))};
}

ProfileWriteResult write_automation_state(
    const std::filesystem::path &profile_path,
    const layerc::core::AutomationProfile &automation_profile,
    const layerc::core::AutomationPersistentValues &values) {
  const auto path = automation_state_path(profile_path);
  if (path.empty() || profile_path.empty())
    return {ProfileWriteError::invalid_input};
  const auto contents =
      layerc::core::serialize_automation_state_json(automation_profile, values);
  if (contents.size() > layerc::core::max_automation_state_bytes)
    return {ProfileWriteError::invalid_input};
  std::error_code path_error;
  const auto absolute = std::filesystem::absolute(path, path_error);
  if (path_error) return {ProfileWriteError::invalid_input};
  // Transient session data: no user-visible `.bak`. The journal still owns
  // crash rollback for the single mutation.
  const std::array mutations{text_mutation(absolute, contents)};
  // Scope on the profile directory (not `_state/`) so this write shares the
  // store's transaction roots with bundle saves and startup recovery.
  return commit_profile_transaction(profile_path.parent_path(), mutations);
}

AutomationCheckpoint checkpoint_automation_state(
    const std::filesystem::path &profile_path,
    const layerc::core::AutomationProfile &automation_profile,
    const layerc::core::AutomationPersistentValues &values,
    std::optional<layerc::core::TimePoint> &due,
    const layerc::core::TimePoint now, const bool force) {
  if (!layerc::core::automation_checkpoint_ready(due, now, force))
    return CheckpointNotDue{};
  auto written =
      write_automation_state(profile_path, automation_profile, values);
  layerc::core::automation_checkpoint_report(due, now, written.ok());
  if (written.failure.has_value()) return *written.failure;
  return CheckpointSaved{};
}

} // namespace layerc::platform
