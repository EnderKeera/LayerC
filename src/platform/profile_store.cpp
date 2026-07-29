#include "layerc/platform/profile_store.hpp"

#include "layerc/core/profile.hpp"

#include <array>
#include <fstream>

#include <shlobj.h>
#include <windows.h>

namespace layerc::platform {

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
  return MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
         FALSE;
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