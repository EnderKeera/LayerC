#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/platform_events.hpp"
#include "layerc/platform/win32_handle.hpp"
#include "profile_store_layout.hpp"

#include <vector>

#include <shlobj.h>
#include <windows.h>

namespace layerc::platform {

std::filesystem::path profile_directory() {
  std::vector<wchar_t> override_path(32'768);
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

ProfileStoreRoots profile_layout_roots(
    const std::filesystem::path &profile_path) {
  const auto parent = profile_path.parent_path();
  return {.config_profiles = parent, .data_profiles = parent, .store = parent};
}

std::filesystem::path profile_layout_final_path(
    const std::filesystem::path &path) {
  const OwnedHandle file(
      CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file.valid()) return {};
  const DWORD required = GetFinalPathNameByHandleW(
      file.get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (required == 0) return {};
  std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U);
  const DWORD length = GetFinalPathNameByHandleW(
      file.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length == 0 || length >= buffer.size()) return {};
  return std::filesystem::path(std::wstring(buffer.data(), length));
}

bool signal_profile_reload() {
  return signal_named_event(profile_reload_event_name);
}

} // namespace layerc::platform
