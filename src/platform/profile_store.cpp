#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/platform_events.hpp"

#include "layerc/core/hash.hpp"
#include "layerc/core/utf8.hpp"

#include <algorithm>
#include <array>
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

std::filesystem::path visual_directory() {
  return profile_directory() / L"_visual";
}

std::filesystem::path
profile_data_directory(const std::filesystem::path &profile_path) {
  return profile_path.parent_path();
}

std::filesystem::path profile_visual_asset_path(
    const std::filesystem::path &profile_path, const std::string_view sha256) {
  if (profile_path.empty() || !layerc::core::is_lowercase_sha256(sha256))
    return {};
  return profile_path.parent_path() / L"_visual" / L"assets" /
         (std::wstring(sha256.begin(), sha256.end()) + L".png");
}

std::filesystem::path
visual_sidecar_path(const std::filesystem::path &profile_path) {
  if (profile_path.filename().empty()) return {};
  return profile_path.parent_path() / L"_visual" / profile_path.filename();
}

std::filesystem::path
automation_sidecar_path(const std::filesystem::path &profile_path) {
  if (profile_path.filename().empty()) return {};
  return profile_path.parent_path() / L"_automation" / profile_path.filename();
}

std::filesystem::path
automation_state_path(const std::filesystem::path &profile_path) {
  if (profile_path.filename().empty()) return {};
  auto filename = profile_path.filename();
  filename += L".automation-state.json";
  return profile_path.parent_path() / L"_state" / filename;
}

std::filesystem::path
companion_sidecar_path(const std::filesystem::path &profile_path) {
  if (profile_path.filename().empty()) return {};
  return profile_path.parent_path() / L"_companion" / profile_path.filename();
}

std::string canonical_profile_key(const std::filesystem::path &path) {
  if (path.empty()) return {};
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
  const auto encoded = layerc::core::try_wide_to_utf8(resolved);
  if (!encoded.has_value()) return {};
  return layerc::core::try_casefold_utf8(*encoded).value_or(std::string{});
}

bool signal_profile_reload() {
  const HANDLE event =
      OpenEventW(EVENT_MODIFY_STATE, FALSE, profile_reload_event_name);
  if (event == nullptr) return false;
  const BOOL signaled = SetEvent(event);
  CloseHandle(event);
  return signaled != FALSE;
}

} // namespace layerc::platform
