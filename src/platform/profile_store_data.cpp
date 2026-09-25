#include "layerc/platform/profile_store.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_file_guard.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <array>
#include <unordered_set>
#include <utility>
#include <vector>

namespace layerc::platform {
namespace {

std::string as_text(const std::span<const std::byte> bytes) {
  if (bytes.empty()) return {};
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

std::filesystem::path sidecar_state_path(const std::filesystem::path &sidecar) {
  auto state = sidecar;
  state += ".state";
  return state;
}

[[nodiscard]] ProfileFileMutation
text_mutation(const std::filesystem::path &target,
              const std::string_view contents) {
  const auto *first = reinterpret_cast<const std::byte *>(contents.data());
  return ProfileFileMutation{
      .target = target,
      .contents = std::vector<std::byte>(first, first + contents.size())};
}

} // namespace

std::optional<std::string>
read_profile_file(const std::filesystem::path &path) {
  return read_durable_text(path, layerc::core::max_profile_json_bytes);
}

// --- Profile directory scan ---

const StoredProfile *ProfileDirectoryScan::find(
    const std::string_view id) const noexcept {
  const auto found = std::find_if(
      profiles.begin(), profiles.end(), [id](const StoredProfile &entry) {
        return entry.profile.id == id;
      });
  return found == profiles.end() ? nullptr : &*found;
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
    result.faults.push_back({.path = path, .message = message});
    if (!result.warning.empty()) result.warning += "; ";
    result.warning += message;
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
                           const ModuleSidecarKind kind) {
  std::filesystem::path primary;
  switch (kind) {
  case ModuleSidecarKind::visual:
    primary = visual_sidecar_path(profile_path);
    break;
  case ModuleSidecarKind::automation:
    primary = automation_sidecar_path(profile_path);
    break;
  case ModuleSidecarKind::companion:
    primary = companion_sidecar_path(profile_path);
    break;
  }
  auto marker = sidecar_state_path(primary);
  return {.primary = primary,
          .backup = recovery_backup_path(primary),
          .marker = std::move(marker)};
}

std::string_view sidecar_marker_contents(const bool active) noexcept {
  return active ? "active\n" : "removed\n";
}

// --- Sidecar store: typed load with primary/backup/state recovery ---
namespace {

constexpr std::size_t max_sidecar_state_bytes = 32;

struct SidecarStateSnapshot {
  bool exists{};
  std::optional<std::string> contents;
};

SidecarStateSnapshot read_sidecar_state(const std::filesystem::path &sidecar) {
  auto read =
      read_durable_file(sidecar_state_path(sidecar), max_sidecar_state_bytes);
  const bool exists = read.status != FileReadStatus::missing;
  return {.exists = exists,
          .contents = read.has_value()
                          ? std::optional<std::string>(as_text(*read))
                          : std::nullopt};
}


// Generic loader shared by every module sidecar. The codec supplies its
// typed parser, byte cap, and diagnostic name; recovery policy lives once.
struct VisualSidecarCodec {
  using Profile = layerc::core::VisualProfile;
  using Error = layerc::core::VisualError;
  static constexpr std::string_view kind = "visual";
  static constexpr std::size_t max_bytes = layerc::core::max_visual_json_bytes;
  static auto parse(const std::string_view source,
                    const layerc::core::GameProfile &base) {
    return layerc::core::parse_visual_profile_json(source, base);
  }
};

struct AutomationSidecarCodec {
  using Profile = layerc::core::AutomationProfile;
  using Error = layerc::core::AutomationError;
  static constexpr std::string_view kind = "automation";
  static constexpr std::size_t max_bytes =
      layerc::core::max_automation_json_bytes;
  static auto parse(const std::string_view source,
                    const layerc::core::GameProfile &base) {
    return layerc::core::parse_automation_profile_json(source, base);
  }
};

struct CompanionSidecarCodec {
  using Profile = layerc::core::CompanionProfile;
  using Error = layerc::core::CompanionError;
  static constexpr std::string_view kind = "companion";
  static constexpr std::size_t max_bytes =
      layerc::core::max_companion_sidecar_bytes;
  static auto parse(const std::string_view source,
                    const layerc::core::GameProfile &base) {
    return layerc::core::parse_companion_profile_json(source, base);
  }
};

template <typename CodecT> class SidecarStore {
public:
  using Profile = typename CodecT::Profile;
  using Error = typename CodecT::Error;

  using LoadResult = ModuleSidecarLoad<Profile, Error>;

  [[nodiscard]] LoadResult load(const std::filesystem::path &sidecar,
                                const layerc::core::GameProfile &base) const {
    const auto backup = recovery_backup_path(sidecar);
    const auto state = read_sidecar_state(sidecar);
    if (state.exists) {
      const auto state_path = sidecar_state_path(sidecar);
      if (!state.contents.has_value())
        return {.state = ModuleSidecarState::invalid,
                .profile = std::nullopt,
                .error = {"$.state", module_message("state could not be read")},
                .source_path = state_path};
      if (*state.contents == sidecar_marker_contents(false))
        return {.state = ModuleSidecarState::absent,
                .profile = std::nullopt,
                .error = {},
                .source_path = state_path};
      if (*state.contents != sidecar_marker_contents(true))
        return {.state = ModuleSidecarState::invalid,
                .profile = std::nullopt,
                .error = {"$.state", module_message("state is invalid")},
                .source_path = state_path};
    }

    const auto primary = read_durable_file(sidecar, CodecT::max_bytes);
    const auto recovery = read_durable_file(backup, CodecT::max_bytes);
    Error primary_error{
        "$", module_message(primary.status == FileReadStatus::too_large
                                ? "exceeds its byte limit"
                                : "could not be read")};
    if (primary.has_value()) {
      auto parsed = CodecT::parse(as_text(*primary), base);
      if (auto *profile = std::get_if<Profile>(&parsed))
        return {.state = ModuleSidecarState::valid,
                .profile = std::move(*profile),
                .error = {},
                .source_path = sidecar};
      primary_error = std::get<Error>(std::move(parsed));
    }
    if (recovery.has_value()) {
      auto parsed = CodecT::parse(as_text(*recovery), base);
      if (auto *profile = std::get_if<Profile>(&parsed))
        return {.state = ModuleSidecarState::recovered_backup,
                .profile = std::move(*profile),
                .error = std::move(primary_error),
                .source_path = backup};
    }

    if (primary.status == FileReadStatus::missing &&
        recovery.status == FileReadStatus::missing)
      return {.state = ModuleSidecarState::absent,
              .profile = std::nullopt,
              .error = {},
              .source_path = sidecar};
    return {.state = ModuleSidecarState::invalid,
            .profile = std::nullopt,
            .error = std::move(primary_error),
            .source_path = sidecar};
  }


private:
  static std::string module_message(const std::string_view detail) {
    return std::string(CodecT::kind) + " sidecar " + std::string(detail);
  }
};

} // namespace

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

  std::vector<ProfileFileMutation> mutations;
  // Keep the previous parseable profile as the last-valid recovery point.
  // Invalid external edits are never promoted into the backup. The
  // user-visible `.bak` is itself a transaction mutation so a rolled-back
  // write cannot leave a backup newer than the primary it claims to protect.
  const auto previous = read_profile_file(absolute);
  if (previous.has_value() &&
      std::holds_alternative<layerc::core::GameProfile>(
          layerc::core::parse_profile_json(*previous)))
    mutations.push_back(text_mutation(recovery_backup_path(absolute), *previous));
  mutations.push_back(text_mutation(absolute, contents));
  return commit_profile_transaction(absolute.parent_path(), mutations);
}

// --- Bundle plan: pure planning (reads only) then one commit ---
namespace {

// N6: one struct instead of five optionals plus a flag parameter. The two
// public entry points (replace vs create-only) differ only in create_only.
struct BundleInputs {
  std::filesystem::path path;
  layerc::core::GameProfile profile;
  std::optional<layerc::core::VisualProfile> visual;
  std::optional<layerc::core::AutomationProfile> automation;
  std::optional<layerc::core::CompanionProfile> companion;
  bool create_only{};
};

struct SidecarPlan {
  SidecarPaths paths;
  std::optional<std::string> contents;
  std::optional<std::string> recovery_contents;
  bool primary_exists{};
  bool backup_exists{};
  bool marker_exists{};
};

struct BundlePlan {
  ProfileStoreRoots roots;
  layerc::core::GameProfile stored_profile;
  std::vector<ProfileFileMutation> mutations;
};

// Prefer the last usable sidecar bytes as the user-visible recovery copy.
// Falls back to the new contents when nothing usable was stored; fails when
// a usable source cannot be re-read.
template <typename Codec>
[[nodiscard]] bool retain_sidecar_recovery(
    SidecarPlan &sidecar,
    const std::optional<layerc::core::GameProfile> &previous_profile) {
  if (!sidecar.contents.has_value()) return true;
  sidecar.recovery_contents = sidecar.contents;
  if (!previous_profile.has_value()) return true;
  const auto loaded =
      SidecarStore<Codec>{}.load(sidecar.paths.primary, *previous_profile);
  if (!loaded.usable()) return true;
  auto source = read_durable_file(loaded.source_path, Codec::max_bytes);
  if (!source.has_value()) return false;
  sidecar.recovery_contents = as_text(*source);
  return true;
}

template <typename T, typename Serialize>
[[nodiscard]] std::optional<std::string>
serialize_if_present(const std::optional<T> &value, Serialize serialize) {
  if (!value.has_value()) return std::nullopt;
  return serialize(*value);
}

[[nodiscard]] std::optional<bool>
path_exists(const std::filesystem::path &target) {
  std::error_code error;
  const bool found = std::filesystem::exists(target, error);
  if (error) return std::nullopt;
  return found;
}

// Validate + assemble: pure planning (reads only, no writes). Nullopt when
// the inputs cannot become a committable bundle.
[[nodiscard]] std::optional<BundlePlan>
plan_profile_bundle(BundleInputs inputs, ProfileWriteError &failure) {
  BundlePlan plan;
  plan.stored_profile = std::move(inputs.profile);
  plan.stored_profile.schema_version = 4;
  plan.stored_profile.modules.clear();
  const auto &path = inputs.path;

  const auto previous_json = read_profile_file(path);
  std::optional<layerc::core::GameProfile> previous_profile;
  if (previous_json.has_value()) {
    auto parsed = layerc::core::parse_profile_json(*previous_json);
    if (auto *profile = std::get_if<layerc::core::GameProfile>(&parsed))
      previous_profile = std::move(*profile);
  }

  // Existing companion data is owned by the profile bundle even when the
  // caller is saving an unrelated base, visual, or automation edit. Until
  // LayerE exposes companion editing/removal, omitted companion state means
  // preserve a valid active sidecar; invalid existing data blocks the save.
  std::optional<layerc::core::CompanionProfile> preserved_companion;
  const auto &previous_base =
      previous_profile.has_value() ? *previous_profile : plan.stored_profile;
  if (!inputs.companion.has_value()) {
    const auto existing = load_companion_sidecar(path, previous_base);
    if (existing.state == ModuleSidecarState::invalid)
      return std::nullopt;
    if (existing.profile.has_value())
      preserved_companion = existing.profile;
  }
  const auto &effective_companion =
      inputs.companion.has_value() ? inputs.companion : preserved_companion;

  if (inputs.visual.has_value()) {
    if (!layerc::core::validate_visual_profile(*inputs.visual, plan.stored_profile)
             .message.empty())
      return std::nullopt;
    plan.stored_profile.modules.push_back(
        {.kind = layerc::core::ProfileModuleKind::visual,
         .schema_version = inputs.visual->schema_version});
  }
  if (inputs.automation.has_value()) {
    if (!layerc::core::validate_automation_profile(*inputs.automation,
                                                   plan.stored_profile)
             .message.empty())
      return std::nullopt;
    plan.stored_profile.modules.push_back(
        {.kind = layerc::core::ProfileModuleKind::automation,
         .schema_version = inputs.automation->schema_version});
  }
  if (effective_companion.has_value()) {
    if (layerc::core::validate_companion_profile(*effective_companion,
                                                 plan.stored_profile))
      return std::nullopt;
    plan.stored_profile.modules.push_back(
        {.kind = layerc::core::ProfileModuleKind::companion,
         .schema_version = effective_companion->schema_version});
  }

  const auto root = path.parent_path();
  const auto visual = sidecar_paths(path, ModuleSidecarKind::visual);
  const auto automation = sidecar_paths(path, ModuleSidecarKind::automation);
  const auto companion = sidecar_paths(path, ModuleSidecarKind::companion);
  if (root.empty() || visual.primary.empty() || automation.primary.empty() ||
      companion.primary.empty())
    return std::nullopt;
  const auto transaction_scope = profile_transaction_scope(root);
  if (!transaction_scope.has_value()) {
    failure = ProfileWriteError::scope_rejected;
    return std::nullopt;
  }
  plan.roots = std::move(*transaction_scope);

  std::array<SidecarPlan, 3> sidecars{{
      {.paths = visual,
       .contents = serialize_if_present(
           inputs.visual, layerc::core::serialize_visual_profile_json),
       .recovery_contents = std::nullopt},
      {.paths = automation,
       .contents = serialize_if_present(
           inputs.automation, layerc::core::serialize_automation_profile_json),
       .recovery_contents = std::nullopt},
      {.paths = companion,
       .contents = serialize_if_present(
           effective_companion,
           layerc::core::serialize_companion_profile_json),
       .recovery_contents = std::nullopt},
  }};
  for (auto &sidecar : sidecars) {
    const auto primary = path_exists(sidecar.paths.primary);
    const auto backup = path_exists(sidecar.paths.backup);
    const auto marker = path_exists(sidecar.paths.marker);
    if (!primary || !backup || !marker) {
      failure = ProfileWriteError::io_failed;
      return std::nullopt;
    }
    sidecar.primary_exists = *primary;
    sidecar.backup_exists = *backup;
    sidecar.marker_exists = *marker;
  }

  if (!retain_sidecar_recovery<VisualSidecarCodec>(sidecars[0],
                                                   previous_profile) ||
      !retain_sidecar_recovery<AutomationSidecarCodec>(sidecars[1],
                                                       previous_profile) ||
      !retain_sidecar_recovery<CompanionSidecarCodec>(sidecars[2],
                                                      previous_profile))
    return std::nullopt;

  std::vector<ProfileFileMutation> mutations;
  mutations.reserve(12);

  // Preserve the last parseable base as the user-visible recovery copy. The
  // transaction journal itself separately owns rollback snapshots.
  if (previous_profile.has_value())
    mutations.push_back(text_mutation(recovery_backup_path(path), *previous_json));

  // Recovery copies are published first. If one is locked or unwritable, the
  // journal rolls back before the new base can become authoritative.
  for (const auto &sidecar : sidecars) {
    if (sidecar.contents.has_value()) {
      mutations.push_back(
          text_mutation(sidecar.paths.backup, *sidecar.recovery_contents));
      mutations.push_back(text_mutation(sidecar.paths.primary, *sidecar.contents));
      mutations.push_back(text_mutation(
          sidecar.paths.marker,
          std::string(sidecar_marker_contents(true))));
    } else if (sidecar.primary_exists || sidecar.backup_exists ||
               sidecar.marker_exists) {
      mutations.push_back(text_mutation(
          sidecar.paths.marker,
          std::string(sidecar_marker_contents(false))));
    }
  }

  mutations.push_back(text_mutation(
      path, layerc::core::serialize_profile_json(plan.stored_profile)));
  mutations.back().must_be_absent = inputs.create_only;
  plan.mutations = std::move(mutations);
  return plan;
}

// The durable tombstone is authoritative. Retain removed sidecar bytes:
// deleting after the transaction releases writer ownership could remove a
// concurrent writer's newly activated sidecar and recovery copy.
ProfileWriteResult write_profile_bundle_impl(BundleInputs inputs) {
  ProfileWriteError failure = ProfileWriteError::invalid_input;
  auto plan = plan_profile_bundle(std::move(inputs), failure);
  if (!plan.has_value()) return {failure};
  return commit_profile_transaction(plan->roots, plan->mutations);
}

} // namespace

ProfileWriteResult write_profile_bundle(
    const std::filesystem::path &path, const layerc::core::GameProfile &profile,
    const std::optional<layerc::core::VisualProfile> &visual_profile,
    const std::optional<layerc::core::AutomationProfile> &automation_profile,
    const std::optional<layerc::core::CompanionProfile> &companion_profile) {
  return write_profile_bundle_impl({.path = path,
                                    .profile = profile,
                                    .visual = visual_profile,
                                    .automation = automation_profile,
                                    .companion = companion_profile,
                                    .create_only = false});
}

ProfileWriteResult write_profile_bundle_create_only(
    const std::filesystem::path &path, const layerc::core::GameProfile &profile,
    const std::optional<layerc::core::VisualProfile> &visual_profile,
    const std::optional<layerc::core::AutomationProfile> &automation_profile,
    const std::optional<layerc::core::CompanionProfile> &companion_profile) {
  return write_profile_bundle_impl({.path = path,
                                    .profile = profile,
                                    .visual = visual_profile,
                                    .automation = automation_profile,
                                    .companion = companion_profile,
                                    .create_only = true});
}

VisualSidecarLoad
load_visual_sidecar(const std::filesystem::path &profile_path,
                    const layerc::core::GameProfile &base_profile) {
  return SidecarStore<VisualSidecarCodec>{}.load(
      sidecar_paths(profile_path, ModuleSidecarKind::visual).primary,
      base_profile);
}


AutomationSidecarLoad
load_automation_sidecar(const std::filesystem::path &profile_path,
                        const layerc::core::GameProfile &base_profile) {
  return SidecarStore<AutomationSidecarCodec>{}.load(
      sidecar_paths(profile_path, ModuleSidecarKind::automation).primary,
      base_profile);
}

CompanionSidecarLoad
load_companion_sidecar(const std::filesystem::path &profile_path,
                       const layerc::core::GameProfile &base_profile) {
  return SidecarStore<CompanionSidecarCodec>{}.load(
      sidecar_paths(profile_path, ModuleSidecarKind::companion).primary,
      base_profile);
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
SidecarMarkerInspection
inspect_sidecar_markers(const std::filesystem::path &sidecar) noexcept {
  const std::array paths{
      sidecar, recovery_backup_path(sidecar), sidecar_state_path(sidecar)};
  bool present{};
  for (const auto &path : paths) {
    std::error_code error;
    present = std::filesystem::exists(path, error) || present;
    if (error) return {.present = false, .error = error};
  }
  return {.present = present, .error = {}};
}

} // namespace layerc::platform
