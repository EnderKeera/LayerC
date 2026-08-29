#include "layerc/platform/profile_store.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

#include <shlobj.h>
#include <windows.h>

namespace layerc::platform {
namespace {

void remove_temporary_file(const std::filesystem::path &path) {
  (void)DeleteFileW(path.c_str());
}

bool write_file_durable(const std::filesystem::path &path,
                        const std::string_view contents) {
  if (contents.size() > std::numeric_limits<DWORD>::max())
    return false;
  const HANDLE file =
      CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  DWORD written{};
  const bool success =
      (contents.empty() ||
       (WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()),
                  &written, nullptr) != FALSE &&
        written == contents.size())) &&
      FlushFileBuffers(file) != FALSE;
  CloseHandle(file);
  if (!success)
    remove_temporary_file(path);
  return success;
}

std::filesystem::path backup_path(const std::filesystem::path &path) {
  auto backup = path;
  backup += L".bak";
  return backup;
}

std::filesystem::path unique_temporary_path(const std::filesystem::path &path) {
  static std::atomic_uint64_t sequence{};
  auto temporary = path;
  temporary +=
      L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
      std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) +
      L".tmp";
  return temporary;
}

} // namespace

std::filesystem::path profile_directory() {
  std::array<wchar_t, 32'768> override_path{};
  const DWORD length =
      GetEnvironmentVariableW(L"LAYERC_PROFILE_DIR", override_path.data(),
                              static_cast<DWORD>(override_path.size()));
  if (length > 0 && length < override_path.size()) {
    return std::filesystem::path(
        std::wstring_view(override_path.data(), length));
  }

  PWSTR local_app_data{};
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                  nullptr, &local_app_data))) {
    return {};
  }
  std::filesystem::path directory(local_app_data);
  CoTaskMemFree(local_app_data);
  return directory / L"LayerC" / L"profiles";
}

std::filesystem::path visual_directory() {
  return profile_directory() / L"_visual";
}

std::filesystem::path
visual_sidecar_path(const std::filesystem::path &profile_path) {
  if (profile_path.filename().empty())
    return {};
  return profile_path.parent_path() / L"_visual" / profile_path.filename();
}

std::filesystem::path
automation_sidecar_path(const std::filesystem::path &profile_path) {
  if (profile_path.filename().empty())
    return {};
  return profile_path.parent_path() / L"_automation" / profile_path.filename();
}

std::filesystem::path
automation_state_path(const std::filesystem::path &profile_path) {
  if (profile_path.filename().empty())
    return {};
  auto filename = profile_path.filename();
  filename += L".automation-state.json";
  return profile_path.parent_path() / L"_state" / filename;
}

std::filesystem::path
companion_sidecar_path(const std::filesystem::path &profile_path) {
  if (profile_path.filename().empty())
    return {};
  return profile_path.parent_path() / L"_companion" / profile_path.filename();
}

std::wstring canonical_profile_key(const std::filesystem::path &path) {
  if (path.empty())
    return {};
  std::wstring resolved;
  const HANDLE file =
      CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    const DWORD required = GetFinalPathNameByHandleW(
        file, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required != 0) {
      std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U);
      const DWORD length = GetFinalPathNameByHandleW(
          file, buffer.data(), static_cast<DWORD>(buffer.size()),
          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      if (length != 0 && length < buffer.size())
        resolved.assign(buffer.data(), length);
    }
    CloseHandle(file);
  }

  if (resolved.empty()) {
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error) {
      error.clear();
      normalized = std::filesystem::absolute(path, error);
    }
    resolved = (error ? path.lexically_normal() : normalized.lexically_normal())
                   .wstring();
  }
  std::replace(resolved.begin(), resolved.end(), L'/', L'\\');
  return lower_copy(std::move(resolved));
}

std::optional<std::string> read_bounded_file(const std::filesystem::path &path,
                                             const std::size_t maximum_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > maximum_bytes) {
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  std::string contents(static_cast<std::size_t>(size), '\0');
  input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (input.gcount() != static_cast<std::streamsize>(contents.size())) {
    return std::nullopt;
  }
  return contents;
}

std::optional<std::string>
read_profile_file(const std::filesystem::path &path) {
  return read_bounded_file(path, layerc::core::max_profile_json_bytes);
}

std::optional<std::string> read_visual_file(const std::filesystem::path &path) {
  return read_bounded_file(path, layerc::core::max_visual_json_bytes);
}
std::optional<std::string>
read_automation_file(const std::filesystem::path &path) {
  return read_bounded_file(path, layerc::core::max_automation_json_bytes);
}
std::optional<std::string>
read_companion_file(const std::filesystem::path &path) {
  return read_bounded_file(path, layerc::core::max_companion_sidecar_bytes);
}

namespace {

bool replace_file_atomically(const std::filesystem::path &path,
                             const std::string_view contents,
                             const bool protect_previous,
                             const std::size_t maximum_bytes) {
  if (contents.size() > maximum_bytes)
    return false;
  std::error_code error;
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
    if (error)
      return false;
  }

  const auto temporary = unique_temporary_path(path);
  if (!write_file_durable(temporary, contents)) {
    remove_temporary_file(temporary);
    return false;
  }

  if (protect_previous) {
    auto backup = backup_path(path);
    const auto backup_temporary = unique_temporary_path(backup);
    if (CopyFileW(path.c_str(), backup_temporary.c_str(), FALSE) == FALSE ||
        MoveFileExW(backup_temporary.c_str(), backup.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
            FALSE) {
      remove_temporary_file(backup_temporary);
      remove_temporary_file(temporary);
      return false;
    }
  }
  if (MoveFileExW(temporary.c_str(), path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
      FALSE) {
    remove_temporary_file(temporary);
    return false;
  }
  return true;
}

constexpr std::string_view sidecar_state_active = "active\n";
constexpr std::string_view sidecar_state_removed = "removed\n";
constexpr std::size_t max_sidecar_state_bytes = 32;

std::filesystem::path sidecar_state_path(const std::filesystem::path &sidecar) {
  auto state = sidecar;
  state += L".state";
  return state;
}

struct SidecarStateSnapshot {
  bool exists{};
  std::optional<std::string> contents;
};

SidecarStateSnapshot read_sidecar_state(const std::filesystem::path &sidecar) {
  const auto state = sidecar_state_path(sidecar);
  std::error_code error;
  const bool exists = std::filesystem::exists(state, error);
  return {.exists = exists && !error,
          .contents = exists && !error
                          ? read_bounded_file(state, max_sidecar_state_bytes)
                          : std::nullopt};
}

bool write_sidecar_state(const std::filesystem::path &sidecar,
                         const std::string_view state) {
  return replace_file_atomically(sidecar_state_path(sidecar), state, false,
                                 max_sidecar_state_bytes);
}

bool restore_file_snapshot(const std::filesystem::path &target,
                           const std::optional<std::string> &contents,
                           const bool existed,
                           const std::size_t maximum_bytes) {
  if (existed) {
    return contents.has_value() &&
           replace_file_atomically(target, *contents, false, maximum_bytes);
  }
  return DeleteFileW(target.c_str()) != FALSE ||
         GetLastError() == ERROR_FILE_NOT_FOUND;
}

// Generic sidecar store shared by every module sidecar (visual, automation,
// and future ones). The codec supplies the typed parse/validate/serialize
// triple, the byte cap, and the module name used in error messages; the
// state marker, backup, and crash-safe rollback are implemented exactly once.
enum class SidecarOutcome { absent, valid, recovered_backup, invalid };

template <typename StateT>
[[nodiscard]] StateT sidecar_state_for(const SidecarOutcome outcome) {
  switch (outcome) {
  case SidecarOutcome::absent:
    return StateT::absent;
  case SidecarOutcome::valid:
    return StateT::valid;
  case SidecarOutcome::recovered_backup:
    return StateT::recovered_backup;
  case SidecarOutcome::invalid:
    return StateT::invalid;
  }
  return StateT::invalid;
}

struct VisualSidecarCodec {
  using Profile = layerc::core::VisualProfile;
  using Error = layerc::core::VisualError;
  static constexpr std::string_view kind = "visual";
  static constexpr std::size_t max_bytes = layerc::core::max_visual_json_bytes;
  static auto parse(const std::string_view source,
                    const layerc::core::GameProfile &base) {
    return layerc::core::parse_visual_profile_json(source, base);
  }
  static auto validate(const Profile &profile,
                       const layerc::core::GameProfile &base) {
    return layerc::core::validate_visual_profile(profile, base);
  }
  static std::string serialize(const Profile &profile) {
    return layerc::core::serialize_visual_profile_json(profile);
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
  static auto validate(const Profile &profile,
                       const layerc::core::GameProfile &base) {
    return layerc::core::validate_automation_profile(profile, base);
  }
  static std::string serialize(const Profile &profile) {
    return layerc::core::serialize_automation_profile_json(profile);
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
  static auto validate(const Profile &profile,
                       const layerc::core::GameProfile &base) {
    return layerc::core::validate_companion_profile(profile, base);
  }
  static std::string serialize(const Profile &profile) {
    return layerc::core::serialize_companion_profile_json(profile);
  }
};

template <typename CodecT> class SidecarStore {
public:
  using Profile = typename CodecT::Profile;
  using Error = typename CodecT::Error;

  struct LoadResult {
    SidecarOutcome outcome{SidecarOutcome::absent};
    std::optional<Profile> profile;
    Error error{};
    std::filesystem::path source_path;
  };

  [[nodiscard]] LoadResult load(const std::filesystem::path &sidecar,
                                const layerc::core::GameProfile &base) const {
    const auto backup = backup_path(sidecar);
    const auto state = read_sidecar_state(sidecar);
    if (state.exists) {
      const auto state_path = sidecar_state_path(sidecar);
      if (!state.contents.has_value())
        return {.outcome = SidecarOutcome::invalid,
                .profile = std::nullopt,
                .error = {"$.state", module_message("state could not be read")},
                .source_path = state_path};
      if (*state.contents == sidecar_state_removed)
        return {.outcome = SidecarOutcome::absent,
                .profile = std::nullopt,
                .error = {},
                .source_path = state_path};
      if (*state.contents != sidecar_state_active)
        return {.outcome = SidecarOutcome::invalid,
                .profile = std::nullopt,
                .error = {"$.state", module_message("state is invalid")},
                .source_path = state_path};
    }

    const auto primary = read_bounded_file(sidecar, CodecT::max_bytes);
    const auto recovery = read_bounded_file(backup, CodecT::max_bytes);
    Error primary_error{"$", module_message("could not be read")};
    if (primary.has_value()) {
      auto parsed = CodecT::parse(*primary, base);
      if (auto *profile = std::get_if<Profile>(&parsed))
        return {.outcome = SidecarOutcome::valid,
                .profile = std::move(*profile),
                .error = {},
                .source_path = sidecar};
      primary_error = std::get<Error>(std::move(parsed));
    }
    if (recovery.has_value()) {
      auto parsed = CodecT::parse(*recovery, base);
      if (auto *profile = std::get_if<Profile>(&parsed))
        return {.outcome = SidecarOutcome::recovered_backup,
                .profile = std::move(*profile),
                .error = std::move(primary_error),
                .source_path = backup};
    }

    std::error_code primary_stat_error;
    std::error_code backup_stat_error;
    const bool primary_exists =
        std::filesystem::exists(sidecar, primary_stat_error);
    const bool backup_exists =
        std::filesystem::exists(backup, backup_stat_error);
    if (!primary_exists && !backup_exists && !primary_stat_error &&
        !backup_stat_error)
      return {.outcome = SidecarOutcome::absent,
              .profile = std::nullopt,
              .error = {},
              .source_path = sidecar};
    return {.outcome = SidecarOutcome::invalid,
            .profile = std::nullopt,
            .error = std::move(primary_error),
            .source_path = sidecar};
  }

  bool write(const std::filesystem::path &sidecar, const Profile &profile,
             const layerc::core::GameProfile &base) const {
    if (!CodecT::validate(profile, base).message.empty() || sidecar.empty())
      return false;
    const auto state = read_sidecar_state(sidecar);
    const auto previous = read_bounded_file(sidecar, CodecT::max_bytes);
    const auto backup = backup_path(sidecar);
    const auto previous_backup = read_bounded_file(backup, CodecT::max_bytes);
    std::error_code primary_error;
    std::error_code backup_error;
    const bool previous_exists =
        std::filesystem::exists(sidecar, primary_error);
    const bool backup_exists = std::filesystem::exists(backup, backup_error);
    if (primary_error || backup_error)
      return false;

    bool protect_previous{};
    const bool marked_removed = state.exists && state.contents.has_value() &&
                                *state.contents == sidecar_state_removed;
    if (previous.has_value() && !marked_removed)
      protect_previous =
          std::holds_alternative<Profile>(CodecT::parse(*previous, base));
    const auto contents = CodecT::serialize(profile);
    // Repairing an invalid primary also refreshes any stale backup before the
    // repaired primary becomes authoritative.
    const bool refresh_backup =
        !protect_previous && (previous_exists || backup_exists);
    if (refresh_backup &&
        !replace_file_atomically(backup, contents, false, CodecT::max_bytes))
      return false;
    if (!replace_file_atomically(sidecar, contents, protect_previous,
                                 CodecT::max_bytes)) {
      if (refresh_backup)
        (void)restore_file_snapshot(backup, previous_backup, backup_exists,
                                    CodecT::max_bytes);
      return false;
    }
    if (write_sidecar_state(sidecar, sidecar_state_active))
      return true;
    (void)restore_file_snapshot(sidecar, previous, previous_exists,
                                CodecT::max_bytes);
    (void)restore_file_snapshot(backup, previous_backup, backup_exists,
                                CodecT::max_bytes);
    (void)restore_file_snapshot(sidecar_state_path(sidecar), state.contents,
                                state.exists, max_sidecar_state_bytes);
    return false;
  }

private:
  static std::string module_message(const std::string_view detail) {
    return std::string(CodecT::kind) + " sidecar " + std::string(detail);
  }
};

} // namespace

bool write_profile_file(const std::filesystem::path &path,
                        const std::string_view contents) {
  // Keep the previous parseable profile as the last-valid recovery point.
  // Invalid external edits are never promoted into the backup.
  const auto previous = read_profile_file(path);
  const bool protect_previous =
      previous.has_value() && std::holds_alternative<layerc::core::GameProfile>(
                                  layerc::core::parse_profile_json(*previous));
  return replace_file_atomically(path, contents, protect_previous,
                                 layerc::core::max_profile_json_bytes);
}

bool write_profile_bundle(
    const std::filesystem::path &path, const layerc::core::GameProfile &profile,
    const std::optional<layerc::core::VisualProfile> &visual_profile,
    const std::optional<layerc::core::AutomationProfile> &automation_profile,
    const std::optional<layerc::core::CompanionProfile> &companion_profile) {
  auto stored_profile = profile;
  stored_profile.schema_version = 4;
  stored_profile.modules.clear();

  // Existing companion data is owned by the profile bundle even when the
  // caller is saving an unrelated base, visual, or automation edit. Until
  // LayerE exposes companion editing/removal, omitted companion state means
  // preserve a valid active sidecar; invalid existing data blocks the save.
  std::optional<layerc::core::CompanionProfile> preserved_companion;
  if (!companion_profile.has_value()) {
    const auto existing = load_companion_sidecar(path, stored_profile);
    if (existing.state == CompanionSidecarState::invalid)
      return false;
    if (existing.profile.has_value())
      preserved_companion = existing.profile;
  }
  const auto &effective_companion =
      companion_profile.has_value() ? companion_profile : preserved_companion;

  if (visual_profile.has_value()) {
    if (!layerc::core::validate_visual_profile(*visual_profile, stored_profile)
             .message.empty())
      return false;
    stored_profile.modules.push_back(
        {.kind = layerc::core::ProfileModuleKind::visual,
         .schema_version = visual_profile->schema_version});
  }
  if (automation_profile.has_value()) {
    if (!layerc::core::validate_automation_profile(*automation_profile,
                                                   stored_profile)
             .message.empty())
      return false;
    stored_profile.modules.push_back(
        {.kind = layerc::core::ProfileModuleKind::automation,
         .schema_version = automation_profile->schema_version});
  }
  if (effective_companion.has_value()) {
    if (!layerc::core::validate_companion_profile(*effective_companion,
                                                  stored_profile)
             .message.empty())
      return false;
    stored_profile.modules.push_back(
        {.kind = layerc::core::ProfileModuleKind::companion,
         .schema_version = effective_companion->schema_version});
  }

  const auto root = path.parent_path();
  const auto visual = visual_sidecar_path(path);
  const auto automation = automation_sidecar_path(path);
  const auto companion = companion_sidecar_path(path);
  if (root.empty() || visual.empty() || automation.empty() || companion.empty())
    return false;

  const auto mutation = [](const std::filesystem::path &target,
                           const std::string &contents) {
    const auto *first = reinterpret_cast<const std::byte *>(contents.data());
    return ProfileFileMutation{
        .target = target,
        .contents = std::vector<std::byte>(first, first + contents.size())};
  };
  const auto exists = [](const std::filesystem::path &target, bool &result) {
    std::error_code error;
    result = std::filesystem::exists(target, error);
    return !error;
  };

  struct SidecarPlan {
    std::filesystem::path sidecar;
    std::filesystem::path backup;
    std::filesystem::path marker;
    std::optional<std::string> contents;
    bool sidecar_exists{};
    bool backup_exists{};
    bool marker_exists{};
  };
  std::array<SidecarPlan, 3> sidecars{{
      {.sidecar = visual,
       .backup = backup_path(visual),
       .marker = sidecar_state_path(visual),
       .contents =
           visual_profile.has_value()
               ? std::optional{layerc::core::serialize_visual_profile_json(
                     *visual_profile)}
               : std::nullopt},
      {.sidecar = automation,
       .backup = backup_path(automation),
       .marker = sidecar_state_path(automation),
       .contents =
           automation_profile.has_value()
               ? std::optional{layerc::core::serialize_automation_profile_json(
                     *automation_profile)}
               : std::nullopt},
      {.sidecar = companion,
       .backup = backup_path(companion),
       .marker = sidecar_state_path(companion),
       .contents =
           effective_companion.has_value()
               ? std::optional{layerc::core::serialize_companion_profile_json(
                     *effective_companion)}
               : std::nullopt},
  }};
  for (auto &sidecar : sidecars) {
    if (!exists(sidecar.sidecar, sidecar.sidecar_exists) ||
        !exists(sidecar.backup, sidecar.backup_exists) ||
        !exists(sidecar.marker, sidecar.marker_exists))
      return false;
  }

  std::vector<ProfileFileMutation> mutations;
  mutations.reserve(12);

  // Preserve the last parseable base as the user-visible recovery copy. The
  // transaction journal itself separately owns rollback snapshots.
  if (const auto previous = read_profile_file(path);
      previous.has_value() && std::holds_alternative<layerc::core::GameProfile>(
                                  layerc::core::parse_profile_json(*previous)))
    mutations.push_back(mutation(backup_path(path), *previous));

  // Recovery copies are published first. If one is locked or unwritable, the
  // journal rolls back before the new base can become authoritative.
  for (const auto &sidecar : sidecars) {
    if (sidecar.contents.has_value()) {
      mutations.push_back(mutation(sidecar.backup, *sidecar.contents));
      mutations.push_back(mutation(sidecar.sidecar, *sidecar.contents));
      mutations.push_back(
          mutation(sidecar.marker, std::string(sidecar_state_active)));
    } else if (sidecar.sidecar_exists || sidecar.backup_exists ||
               sidecar.marker_exists) {
      mutations.push_back(
          mutation(sidecar.marker, std::string(sidecar_state_removed)));
    }
  }

  mutations.push_back(
      mutation(path, layerc::core::serialize_profile_json(stored_profile)));
  if (!commit_profile_transaction(root, mutations))
    return false;

  // A durable removed marker is authoritative. Physical deletion is only
  // cleanup, so a sharing violation cannot reactivate a stale module or turn a
  // committed bundle into a reported failure.
  for (const auto &sidecar : sidecars) {
    if (!sidecar.contents.has_value()) {
      (void)DeleteFileW(sidecar.sidecar.c_str());
      (void)DeleteFileW(sidecar.backup.c_str());
    }
  }
  return true;
}
VisualSidecarLoad
load_visual_sidecar(const std::filesystem::path &profile_path,
                    const layerc::core::GameProfile &base_profile) {
  const auto result = SidecarStore<VisualSidecarCodec>{}.load(
      visual_sidecar_path(profile_path), base_profile);
  return {.state = sidecar_state_for<VisualSidecarState>(result.outcome),
          .profile = std::move(result.profile),
          .error = std::move(result.error),
          .source_path = std::move(result.source_path)};
}

bool write_visual_sidecar(const std::filesystem::path &profile_path,
                          const layerc::core::VisualProfile &visual_profile,
                          const layerc::core::GameProfile &base_profile) {
  return SidecarStore<VisualSidecarCodec>{}.write(
      visual_sidecar_path(profile_path), visual_profile, base_profile);
}

AutomationSidecarLoad
load_automation_sidecar(const std::filesystem::path &profile_path,
                        const layerc::core::GameProfile &base_profile) {
  const auto result = SidecarStore<AutomationSidecarCodec>{}.load(
      automation_sidecar_path(profile_path), base_profile);
  return {.state = sidecar_state_for<AutomationSidecarState>(result.outcome),
          .profile = std::move(result.profile),
          .error = std::move(result.error),
          .source_path = std::move(result.source_path)};
}

CompanionSidecarLoad
load_companion_sidecar(const std::filesystem::path &profile_path,
                       const layerc::core::GameProfile &base_profile) {
  const auto result = SidecarStore<CompanionSidecarCodec>{}.load(
      companion_sidecar_path(profile_path), base_profile);
  return {.state = sidecar_state_for<CompanionSidecarState>(result.outcome),
          .profile = std::move(result.profile),
          .error = std::move(result.error),
          .source_path = std::move(result.source_path)};
}

AutomationStateLoad load_automation_state(
    const std::filesystem::path &profile_path,
    const layerc::core::AutomationProfile &automation_profile) {
  const auto path = automation_state_path(profile_path);
  std::error_code existence_error;
  if (!std::filesystem::exists(path, existence_error)) {
    if (!existence_error)
      return {};
    return {.values = {},
            .error = layerc::core::AutomationError{
                "$", "automation state existence check failed"}};
  }
  const auto contents =
      read_bounded_file(path, layerc::core::max_automation_state_bytes);
  if (!contents.has_value())
    return {.values = {},
            .error = layerc::core::AutomationError{
                "$", "automation state could not be read"}};
  auto parsed =
      layerc::core::parse_automation_state_json(*contents, automation_profile);
  if (auto *values =
          std::get_if<layerc::core::AutomationPersistentValues>(&parsed))
    return {.values = std::move(*values), .error = std::nullopt};
  return {.values = {},
          .error = std::get<layerc::core::AutomationError>(std::move(parsed))};
}

bool write_automation_state(
    const std::filesystem::path &profile_path,
    const layerc::core::AutomationProfile &automation_profile,
    const layerc::core::AutomationPersistentValues &values) {
  const auto path = automation_state_path(profile_path);
  if (path.empty())
    return false;
  return replace_file_atomically(
      path,
      layerc::core::serialize_automation_state_json(automation_profile, values),
      false, layerc::core::max_automation_state_bytes);
}
bool has_stale_sidecar_markers(const std::filesystem::path &sidecar,
                               std::error_code &out_error) noexcept {
  auto backup = sidecar;
  backup += L".bak";
  const auto state = sidecar_state_path(sidecar);
  std::error_code ec;
  const bool sidecar_exists = std::filesystem::exists(sidecar, ec);
  if (ec) {
    out_error = ec;
    return false;
  }
  const bool backup_exists = std::filesystem::exists(backup, ec);
  if (ec) {
    out_error = ec;
    return false;
  }
  const bool state_exists = std::filesystem::exists(state, ec);
  if (ec) {
    out_error = ec;
    return false;
  }
  out_error.clear();
  return sidecar_exists || backup_exists || state_exists;
}

bool signal_profile_reload() {
  const HANDLE event =
      OpenEventW(EVENT_MODIFY_STATE, FALSE, profile_reload_event_name);
  if (event == nullptr) {
    return false;
  }
  const BOOL signaled = SetEvent(event);
  CloseHandle(event);
  return signaled != FALSE;
}

} // namespace layerc::platform
