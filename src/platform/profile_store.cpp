#include "layerc/platform/profile_store.hpp"

#include "layerc/core/profile.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <utility>
#include <vector>

#include <shlobj.h>
#include <windows.h>

namespace layerc::platform {
namespace {

std::wstring invariant_lower(std::wstring value) {
  if (value.empty()) return value;
  const int required = LCMapStringEx(
      LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
  if (required > 0) {
    std::wstring lowered(static_cast<std::size_t>(required), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
                      static_cast<int>(value.size()), lowered.data(), required,
                      nullptr, nullptr, 0) != 0)
      return lowered;
  }
  CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
  return value;
}

void remove_temporary_file(const std::filesystem::path &path) {
  (void)DeleteFileW(path.c_str());
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
  return invariant_lower(std::move(resolved));
}

std::optional<std::string>
read_profile_file(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > layerc::core::max_profile_json_bytes) {
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

bool write_profile_file(const std::filesystem::path &path,
                        const std::string_view contents) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }

  auto temporary = path;
  temporary += L".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      return false;
    }
    output.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    if (!output) {
      return false;
    }
  }

  // Keep the previous parseable profile as the last-valid recovery point.
  // Invalid external edits are never promoted into the backup.
  if (const auto previous = read_profile_file(path); previous.has_value() &&
      std::holds_alternative<layerc::core::GameProfile>(
          layerc::core::parse_profile_json(*previous))) {
    auto backup = path;
    backup += L".bak";
    auto backup_temporary = backup;
    backup_temporary += L".tmp";
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
