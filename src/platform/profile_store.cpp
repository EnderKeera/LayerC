#include "layerc/platform/profile_store.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <utility>
#include <vector>

#include <shlobj.h>
#include <windows.h>

namespace layerc::platform {
namespace {

void remove_temporary_file(const std::filesystem::path &path) {
  (void)DeleteFileW(path.c_str());
}

std::filesystem::path backup_path(const std::filesystem::path &path) {
  auto backup = path;
  backup += L".bak";
  return backup;
}

std::filesystem::path
unique_temporary_path(const std::filesystem::path &path) {
  static std::atomic_uint64_t sequence{};
  auto temporary = path;
  temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
               std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) +
               L".tmp";
  return temporary;
}

} // namespace

std::filesystem::path profile_directory() {
  std::array<wchar_t, 32'768> override_path{};
  const DWORD length = GetEnvironmentVariableW(
      L"LAYERC_PROFILE_DIR", override_path.data(),
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
  if (profile_path.filename().empty()) return {};
  return profile_path.parent_path() / L"_visual" / profile_path.filename();
}

std::wstring canonical_profile_key(const std::filesystem::path &path) {
  if (path.empty()) return {};
  std::wstring resolved;
  const HANDLE file = CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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

std::optional<std::string>
read_bounded_file(const std::filesystem::path &path,
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

std::optional<std::string> read_visual_file(
    const std::filesystem::path &path) {
  return read_bounded_file(path, layerc::core::max_visual_json_bytes);
}

namespace {

bool replace_file_atomically(const std::filesystem::path &path,
                             const std::string_view contents,
                             const bool protect_previous,
                             const std::size_t maximum_bytes) {
  if (contents.size() > maximum_bytes) return false;
  std::error_code error;
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
    if (error) return false;
  }

  const auto temporary = unique_temporary_path(path);
  bool temporary_written{};
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (output) {
      output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
      temporary_written = static_cast<bool>(output);
    }
  }
  if (!temporary_written) {
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
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
    remove_temporary_file(temporary);
    return false;
  }
  return true;
}

constexpr std::string_view visual_state_active = "active\n";
constexpr std::string_view visual_state_removed = "removed\n";
constexpr std::size_t max_visual_state_bytes = 32;

std::filesystem::path visual_state_path(
    const std::filesystem::path &sidecar) {
  auto state = sidecar;
  state += L".state";
  return state;
}

struct VisualStateSnapshot {
  bool exists{};
  std::optional<std::string> contents;
};

VisualStateSnapshot read_visual_state(
    const std::filesystem::path &sidecar) {
  const auto state = visual_state_path(sidecar);
  std::error_code error;
  const bool exists = std::filesystem::exists(state, error);
  return {.exists = exists && !error,
          .contents = exists && !error
                          ? read_bounded_file(state, max_visual_state_bytes)
                          : std::nullopt};
}

bool write_visual_state(const std::filesystem::path &sidecar,
                        const std::string_view state) {
  return replace_file_atomically(visual_state_path(sidecar), state, false,
                                 max_visual_state_bytes);
}

bool restore_visual_state(const std::filesystem::path &sidecar,
                          const VisualStateSnapshot &snapshot) {
  const auto state = visual_state_path(sidecar);
  if (snapshot.exists)
    return snapshot.contents.has_value() &&
           replace_file_atomically(state, *snapshot.contents, false,
                                   max_visual_state_bytes);
  return DeleteFileW(state.c_str()) != FALSE ||
         GetLastError() == ERROR_FILE_NOT_FOUND;
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

} // namespace

bool write_profile_file(const std::filesystem::path &path,
                        const std::string_view contents) {
  // Keep the previous parseable profile as the last-valid recovery point.
  // Invalid external edits are never promoted into the backup.
  const auto previous = read_profile_file(path);
  const bool protect_previous =
      previous.has_value() &&
      std::holds_alternative<layerc::core::GameProfile>(
          layerc::core::parse_profile_json(*previous));
  return replace_file_atomically(path, contents, protect_previous,
                                 layerc::core::max_profile_json_bytes);
}

bool write_profile_bundle(
    const std::filesystem::path &path,
    const layerc::core::GameProfile &profile,
    const std::optional<layerc::core::VisualProfile> &visual_profile) {
  auto stored_profile = profile;
  stored_profile.schema_version = 3;
  stored_profile.modules.clear();
  if (visual_profile.has_value()) {
    stored_profile.modules.push_back(
        {.kind = layerc::core::ProfileModuleKind::visual,
         .schema_version = visual_profile->schema_version});
  }
  const auto profile_json =
      layerc::core::serialize_profile_json(stored_profile);
  const auto sidecar = visual_sidecar_path(path);
  if (sidecar.empty()) return false;
  if (!visual_profile.has_value()) {
    const auto sidecar_backup = backup_path(sidecar);
    const auto previous_state = read_visual_state(sidecar);
    std::error_code profile_exists_error;
    (void)std::filesystem::exists(path, profile_exists_error);
    std::error_code primary_error;
    std::error_code backup_error;
    const bool primary_exists =
        std::filesystem::exists(sidecar, primary_error);
    const bool backup_exists =
        std::filesystem::exists(sidecar_backup, backup_error);
    if (profile_exists_error || primary_error || backup_error) return false;
    if (!primary_exists && !backup_exists && !previous_state.exists)
      return write_profile_file(path, profile_json);

    // Publish the removal intent first. The marker remains authoritative, so
    // a crash before or after the base replacement can never reactivate stale
    // sidecars or turn an ID change into a blocking mismatch.
    if (!write_visual_state(sidecar, visual_state_removed)) return false;
    if (!write_profile_file(path, profile_json)) {
      (void)restore_visual_state(sidecar, previous_state);
      return false;
    }
    // Physical cleanup is best effort. A sharing violation can leave the old
    // bytes on disk, but the durable removed marker keeps them inactive.
    (void)DeleteFileW(sidecar.c_str());
    (void)DeleteFileW(sidecar_backup.c_str());
    return true;
  }
  if (!layerc::core::validate_visual_profile(*visual_profile, stored_profile)
           .message.empty())
    return false;

  const auto visual_json =
      layerc::core::serialize_visual_profile_json(*visual_profile);

  const auto previous_profile_json = read_profile_file(path);
  const auto previous_visual_json = read_visual_file(sidecar);
  auto sidecar_backup = backup_path(sidecar);
  const auto previous_visual_backup_json =
      read_visual_file(sidecar_backup);
  const auto previous_state = read_visual_state(sidecar);
  const bool previous_marked_removed =
      previous_state.exists && previous_state.contents.has_value() &&
      *previous_state.contents == visual_state_removed;
  std::error_code profile_existence_error;
  std::error_code visual_existence_error;
  std::error_code backup_existence_error;
  const bool previous_profile_exists =
      std::filesystem::exists(path, profile_existence_error);
  const bool previous_visual_exists =
      std::filesystem::exists(sidecar, visual_existence_error);
  const bool previous_visual_backup_exists =
      std::filesystem::exists(sidecar_backup, backup_existence_error);
  if (profile_existence_error || visual_existence_error ||
      backup_existence_error)
    return false;

  const layerc::core::GameProfile *previous_profile{};
  layerc::core::ProfileResult previous_profile_result{
      layerc::core::ProfileError{}};
  if (previous_profile_json.has_value()) {
    previous_profile_result =
        layerc::core::parse_profile_json(*previous_profile_json);
    previous_profile =
        std::get_if<layerc::core::GameProfile>(&previous_profile_result);
  }
  bool previous_primary_pair_valid{};
  bool previous_backup_pair_valid{};
  if (previous_profile != nullptr && previous_visual_json.has_value()) {
    previous_primary_pair_valid =
        std::holds_alternative<layerc::core::VisualProfile>(
            layerc::core::parse_visual_profile_json(*previous_visual_json,
                                                    *previous_profile));
  }
  if (previous_profile != nullptr &&
      previous_visual_backup_json.has_value()) {
    previous_backup_pair_valid =
        std::holds_alternative<layerc::core::VisualProfile>(
            layerc::core::parse_visual_profile_json(
                *previous_visual_backup_json, *previous_profile));
  }
  if (previous_marked_removed) {
    previous_primary_pair_valid = false;
    previous_backup_pair_valid = false;
  }
  bool previous_primary_valid_for_new_profile{};
  if (previous_visual_json.has_value()) {
    previous_primary_valid_for_new_profile =
        std::holds_alternative<layerc::core::VisualProfile>(
            layerc::core::parse_visual_profile_json(*previous_visual_json,
                                                    profile));
  }
  const bool refresh_backup_after_commit =
      previous_marked_removed ||
      ((!previous_primary_pair_valid ||
        !previous_primary_valid_for_new_profile) &&
       (previous_visual_json.has_value() ||
        previous_visual_backup_json.has_value()));
  const bool refresh_backup_before_commit =
      refresh_backup_after_commit && previous_primary_pair_valid;
  // If a valid primary exists, it is the safest rollback recovery copy. This
  // also replaces an older stale backup rather than resurrecting it later.
  const auto rollback_visual_backup_json =
      previous_primary_pair_valid ? previous_visual_json
                                  : previous_visual_backup_json;
  const bool rollback_visual_backup_exists =
      previous_primary_pair_valid || previous_visual_backup_exists;
  const auto restore_base_and_visual = [&](const bool restore_backup,
                                           const bool restore_state) {
    (void)restore_file_snapshot(
        path, previous_profile_json, previous_profile_exists,
        layerc::core::max_profile_json_bytes);
    (void)restore_file_snapshot(
        sidecar, previous_visual_json, previous_visual_exists,
        layerc::core::max_visual_json_bytes);
    if (restore_backup)
      (void)restore_file_snapshot(
          sidecar_backup, rollback_visual_backup_json,
          rollback_visual_backup_exists,
          layerc::core::max_visual_json_bytes);
    if (restore_state)
      (void)restore_visual_state(sidecar, previous_state);
  };

  // Check and publish the refreshed recovery copy before touching a valid
  // primary/base pair. A locked or unwritable .bak therefore fails with the
  // active bundle completely untouched.
  if (refresh_backup_before_commit &&
      !replace_file_atomically(sidecar_backup, visual_json, false,
                               layerc::core::max_visual_json_bytes))
    return false;

  if (previous_primary_pair_valid || previous_backup_pair_valid) {
    // Replace the visual file first and protect its matching old copy. If the
    // process stops before the base replacement, the loader can recover that
    // old copy; after the base replacement the new primary is valid.
    if (!replace_file_atomically(sidecar, visual_json,
                                 previous_primary_pair_valid,
                                 layerc::core::max_visual_json_bytes)) {
      if (refresh_backup_before_commit)
        restore_base_and_visual(true, false);
      return false;
    }
    if (write_profile_file(path, profile_json)) {
      if (refresh_backup_after_commit && !refresh_backup_before_commit &&
          !replace_file_atomically(sidecar_backup, visual_json, false,
                                   layerc::core::max_visual_json_bytes)) {
        restore_base_and_visual(true, false);
        return false;
      }
      if (write_visual_state(sidecar, visual_state_active)) return true;
      restore_base_and_visual(refresh_backup_after_commit, true);
      return false;
    }
    (void)restore_file_snapshot(
        sidecar, previous_visual_json, previous_visual_exists,
        layerc::core::max_visual_json_bytes);
    if (refresh_backup_before_commit)
      restore_base_and_visual(true, false);
    return false;
  }

  // With no valid old pair, publish a removed marker before replacing either
  // file. Every crash point then loads as input-only until the new matching
  // base and visual files are both durable and the active marker is published.
  if (!write_visual_state(sidecar, visual_state_removed)) return false;
  if (!write_profile_file(path, profile_json)) {
    (void)restore_visual_state(sidecar, previous_state);
    return false;
  }
  if (replace_file_atomically(sidecar, visual_json, false,
                              layerc::core::max_visual_json_bytes)) {
    if (refresh_backup_after_commit &&
        !replace_file_atomically(sidecar_backup, visual_json, false,
                                 layerc::core::max_visual_json_bytes)) {
      restore_base_and_visual(true, true);
      return false;
    }
    if (write_visual_state(sidecar, visual_state_active)) return true;
    restore_base_and_visual(refresh_backup_after_commit, true);
    return false;
  }
  (void)restore_file_snapshot(path, previous_profile_json,
                              previous_profile_exists,
                              layerc::core::max_profile_json_bytes);
  (void)restore_visual_state(sidecar, previous_state);
  return false;
}

VisualSidecarLoad load_visual_sidecar(
    const std::filesystem::path &profile_path,
    const layerc::core::GameProfile &base_profile) {
  const auto sidecar = visual_sidecar_path(profile_path);
  auto backup = backup_path(sidecar);
  const auto state = read_visual_state(sidecar);
  if (state.exists) {
    if (!state.contents.has_value())
      return {.state = VisualSidecarState::invalid,
              .error = {"$.state", "visual sidecar state could not be read"},
              .source_path = visual_state_path(sidecar)};
    if (*state.contents == visual_state_removed)
      return {.state = VisualSidecarState::absent,
              .source_path = visual_state_path(sidecar)};
    if (*state.contents != visual_state_active)
      return {.state = VisualSidecarState::invalid,
              .error = {"$.state", "visual sidecar state is invalid"},
              .source_path = visual_state_path(sidecar)};
  }

  auto primary_contents = read_visual_file(sidecar);
  auto backup_contents = read_visual_file(backup);

  layerc::core::VisualError primary_error{
      "$", "visual sidecar could not be read"};
  bool primary_parsed{};
  if (primary_contents.has_value()) {
    auto parsed = layerc::core::parse_visual_profile_json(*primary_contents, base_profile);
    if (auto *profile = std::get_if<layerc::core::VisualProfile>(&parsed))
      return {.state = VisualSidecarState::valid,
              .profile = std::move(*profile),
              .source_path = sidecar};
    primary_error = std::get<layerc::core::VisualError>(std::move(parsed));
    primary_parsed = true;
  }

  if (backup_contents.has_value()) {
    auto parsed = layerc::core::parse_visual_profile_json(*backup_contents, base_profile);
    if (auto *profile = std::get_if<layerc::core::VisualProfile>(&parsed))
      return {.state = VisualSidecarState::recovered_backup,
              .profile = std::move(*profile),
              .error = std::move(primary_error),
              .source_path = backup};
  }

  if (!primary_contents.has_value() && !backup_contents.has_value()) {
    std::error_code ec1, ec2;
    const bool primary_exists = std::filesystem::exists(sidecar, ec1);
    const bool backup_exists = std::filesystem::exists(backup, ec2);
    if (!primary_exists && !backup_exists && !ec1 && !ec2)
      return {.state = VisualSidecarState::absent, .source_path = sidecar};
  }
  // At this point primary was either missing-unreadable or invalid, and backup
  // did not yield a valid profile.
  (void)primary_parsed;
  return {.state = VisualSidecarState::invalid,
          .error = std::move(primary_error),
          .source_path = sidecar};
}

bool write_visual_sidecar(
    const std::filesystem::path &profile_path,
    const layerc::core::VisualProfile &visual_profile,
    const layerc::core::GameProfile &base_profile) {
  if (!layerc::core::validate_visual_profile(visual_profile, base_profile)
           .message.empty())
    return false;
  const auto sidecar = visual_sidecar_path(profile_path);
  if (sidecar.empty()) return false;
  const auto state = read_visual_state(sidecar);
  const auto previous = read_visual_file(sidecar);
  auto backup = backup_path(sidecar);
  const auto previous_backup = read_visual_file(backup);
  std::error_code previous_error;
  std::error_code backup_error;
  const bool previous_exists = std::filesystem::exists(sidecar, previous_error);
  const bool backup_exists = std::filesystem::exists(backup, backup_error);
  if (previous_error || backup_error) return false;
  bool protect_previous{};
  const bool marked_removed =
      state.exists && state.contents.has_value() &&
      *state.contents == visual_state_removed;
  if (previous.has_value() && !marked_removed) {
    protect_previous = std::holds_alternative<layerc::core::VisualProfile>(
        layerc::core::parse_visual_profile_json(*previous, base_profile));
  }
  const auto contents =
      layerc::core::serialize_visual_profile_json(visual_profile);
  // Repairing an invalid primary also refreshes any stale backup before the
  // repaired primary becomes authoritative.
  const bool refresh_backup = !protect_previous &&
                              (previous_exists || backup_exists);
  if (refresh_backup &&
      !replace_file_atomically(backup, contents, false,
                               layerc::core::max_visual_json_bytes))
    return false;
  if (!replace_file_atomically(sidecar, contents, protect_previous,
                               layerc::core::max_visual_json_bytes)) {
    if (refresh_backup) {
      if (previous_backup.has_value())
        (void)replace_file_atomically(backup, *previous_backup, false,
                                      layerc::core::max_visual_json_bytes);
      else
        (void)DeleteFileW(backup.c_str());
    }
    return false;
  }
  if (write_visual_state(sidecar, visual_state_active)) return true;
  (void)restore_file_snapshot(sidecar, previous, previous_exists,
                              layerc::core::max_visual_json_bytes);
  (void)restore_file_snapshot(backup, previous_backup, backup_exists,
                              layerc::core::max_visual_json_bytes);
  (void)restore_visual_state(sidecar, state);
  return false;
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
