#include "layerc/platform/window_query.hpp"

#include <appmodel.h>

#include <cstddef>
#include <filesystem>
#include <string_view>
#include <vector>

#include <shellscalingapi.h>

namespace layerc::platform {
namespace {

constexpr DWORD capture_exclusion_affinity = 0x00000011;

std::filesystem::path sibling_image_path(const wchar_t *filename) {
  std::vector<wchar_t> module(32'768);
  const DWORD length = GetModuleFileNameW(nullptr, module.data(),
                                          static_cast<DWORD>(module.size()));
  if (length == 0 || length >= module.size())
    return {};
  return std::filesystem::path(std::wstring_view(module.data(), length))
             .parent_path() /
         filename;
}

} // namespace

bool process_image_equals(const HANDLE process,
                          const std::filesystem::path &expected) {
  std::vector<wchar_t> actual(32'768);
  DWORD length = static_cast<DWORD>(actual.size());
  if (QueryFullProcessImageNameW(process, 0, actual.data(), &length) == FALSE ||
      length == 0)
    return false;
  std::error_code error;
  const bool equivalent = std::filesystem::equivalent(
      expected, std::filesystem::path(std::wstring_view(actual.data(), length)),
      error);
  return !error && equivalent;
}

bool is_sibling_process_image(const DWORD process_id,
                              const wchar_t *const filename) {
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (process == nullptr) return false;
  const bool sibling = process_image_equals(process, sibling_image_path(filename));
  CloseHandle(process);
  return sibling;
}

bool apply_capture_exclusion(const HWND window, const bool enabled) {
  if (window == nullptr || !IsWindow(window))
    return false;
  return SetWindowDisplayAffinity(window, enabled ? capture_exclusion_affinity
                                                  : WDA_NONE) != FALSE;
}

bool same_user_as_current_process(const HANDLE process) {
  HANDLE current_token{};
  HANDLE other_token{};
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &current_token) ==
          FALSE ||
      OpenProcessToken(process, TOKEN_QUERY, &other_token) == FALSE) {
    if (current_token != nullptr)
      CloseHandle(current_token);
    if (other_token != nullptr)
      CloseHandle(other_token);
    return false;
  }
  const auto token_user = [](const HANDLE token,
                             std::vector<std::byte> &storage) -> PSID {
    DWORD required{};
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
      return nullptr;
    storage.resize(required);
    if (GetTokenInformation(token, TokenUser, storage.data(), required,
                            &required) == FALSE)
      return nullptr;
    return reinterpret_cast<TOKEN_USER *>(storage.data())->User.Sid;
  };
  std::vector<std::byte> current_storage;
  std::vector<std::byte> other_storage;
  const PSID current_sid = token_user(current_token, current_storage);
  const PSID other_sid = token_user(other_token, other_storage);
  const bool matches = current_sid != nullptr && other_sid != nullptr &&
                       EqualSid(current_sid, other_sid) != FALSE;
  CloseHandle(other_token);
  CloseHandle(current_token);
  return matches;
}

UINT monitor_dpi_for_rect(const RECT &rect) {
  const HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
  UINT dpi_x{};
  UINT dpi_y{};
  if (monitor != nullptr &&
      SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) &&
      dpi_x != 0)
    return dpi_x;
  const UINT system_dpi = GetDpiForSystem();
  return system_dpi == 0 ? 96U : system_dpi;
}

bool is_gamebar_overlay_process(const DWORD process_id) {
  if (process_id == 0)
    return false;
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (process == nullptr)
    return false;

  UINT32 family_length{};
  const LONG probe = GetPackageFamilyName(process, &family_length, nullptr);
  if (probe != ERROR_INSUFFICIENT_BUFFER || family_length <= 1U) {
    CloseHandle(process);
    return false;
  }
  std::vector<wchar_t> family(family_length);
  const LONG result =
      GetPackageFamilyName(process, &family_length, family.data());
  CloseHandle(process);
  return result == ERROR_SUCCESS &&
         std::wstring_view(family.data()) ==
             L"Microsoft.XboxGamingOverlay_8wekyb3d8bbwe";
}

std::string window_session_key(const HWND window) {
  if (window == nullptr)
    return "global";
  return "window:" +
         std::to_string(reinterpret_cast<std::uintptr_t>(window));
}

} // namespace layerc::platform
