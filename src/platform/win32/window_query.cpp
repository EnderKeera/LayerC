#include "layerc/platform/window_query.hpp"

#include "layerc/platform/win32_handle.hpp"

#include <appmodel.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <string_view>
#include <vector>

#include <shellscalingapi.h>

namespace layerc::platform {
namespace {

constexpr DWORD capture_exclusion_affinity = 0x00000011;

} // namespace

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

std::wstring window_text(const HWND window) {
  const int length = GetWindowTextLengthW(window);
  if (length <= 0) return {};
  std::wstring output(static_cast<std::size_t>(length) + 1U, L'\0');
  const int copied =
      GetWindowTextW(window, output.data(), static_cast<int>(output.size()));
  output.resize(static_cast<std::size_t>(std::max(copied, 0)));
  return output;
}

std::wstring window_class(const HWND window) {
  std::array<wchar_t, 256> value{};
  const int copied =
      GetClassNameW(window, value.data(), static_cast<int>(value.size()));
  return copied > 0
             ? std::wstring(value.data(), static_cast<std::size_t>(copied))
             : std::wstring{};
}

std::wstring process_image_path(const HANDLE process) {
  std::vector<wchar_t> path(32'768);
  DWORD length = static_cast<DWORD>(path.size());
  if (QueryFullProcessImageNameW(process, 0, path.data(), &length) == FALSE)
    return {};
  return std::wstring(path.data(), length);
}

bool process_image_equals(const HANDLE process,
                          const std::filesystem::path &expected) {
  const auto actual = process_image_path(process);
  if (actual.empty()) return false;
  std::error_code error;
  const bool equivalent =
      std::filesystem::equivalent(expected, std::filesystem::path(actual), error);
  return !error && equivalent;
}

bool is_sibling_process_image(const DWORD process_id,
                              const wchar_t *const filename) {
  const OwnedHandle process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
  return process.valid() &&
         process_image_equals(process.get(), sibling_image_path(filename));
}

bool same_session_as_current_process(const DWORD process_id) {
  DWORD peer_session{};
  DWORD current_session{};
  return ProcessIdToSessionId(process_id, &peer_session) != FALSE &&
         ProcessIdToSessionId(GetCurrentProcessId(), &current_session) !=
             FALSE &&
         peer_session == current_session;
}

bool apply_capture_exclusion(const HWND window, const bool enabled) {
  if (window == nullptr || !IsWindow(window))
    return false;
  return SetWindowDisplayAffinity(window, enabled ? capture_exclusion_affinity
                                                  : WDA_NONE) != FALSE;
}

bool same_user_as_current_process(const HANDLE process) {
  HANDLE raw_current{};
  HANDLE raw_other{};
  const bool opened =
      OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_current) !=
          FALSE &&
      OpenProcessToken(process, TOKEN_QUERY, &raw_other) != FALSE;
  const OwnedHandle current_token(raw_current);
  const OwnedHandle other_token(raw_other);
  if (!opened) return false;
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
  const PSID current_sid = token_user(current_token.get(), current_storage);
  const PSID other_sid = token_user(other_token.get(), other_storage);
  return current_sid != nullptr && other_sid != nullptr &&
         EqualSid(current_sid, other_sid) != FALSE;
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
  const OwnedHandle process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
  if (!process.valid())
    return false;

  UINT32 family_length{};
  const LONG probe =
      GetPackageFamilyName(process.get(), &family_length, nullptr);
  if (probe != ERROR_INSUFFICIENT_BUFFER || family_length <= 1U)
    return false;
  std::vector<wchar_t> family(family_length);
  const LONG result =
      GetPackageFamilyName(process.get(), &family_length, family.data());
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
