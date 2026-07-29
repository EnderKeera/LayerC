#include "layerc/platform/windows.hpp"

#include "layerc/platform/text.hpp"

#include <algorithm>
#include <array>
#include <filesystem>

namespace layerc::platform {
namespace {

std::wstring process_path_for_window(const HWND window, DWORD &process_id) {
  GetWindowThreadProcessId(window, &process_id);
  if (process_id == 0) {
    return {};
  }
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (process == nullptr) {
    return {};
  }

  std::array<wchar_t, 32'768> path{};
  DWORD length = static_cast<DWORD>(path.size());
  const BOOL succeeded =
      QueryFullProcessImageNameW(process, 0, path.data(), &length);
  CloseHandle(process);
  return succeeded ? std::wstring(path.data(), length) : std::wstring{};
}

std::wstring window_text(const HWND window) {
  const int length = GetWindowTextLengthW(window);
  if (length <= 0) {
    return {};
  }
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

bool is_shell_window(const HWND window) {
  const auto klass = window_class(window);
  return klass == L"Shell_TrayWnd" || klass == L"Progman" ||
         klass == L"WorkerW";
}

bool is_eligible_window(const HWND window) {
  if (window == nullptr || !IsWindow(window) ||
      GetAncestor(window, GA_ROOT) != window || !IsWindowVisible(window) ||
      GetWindow(window, GW_OWNER) != nullptr || window == GetShellWindow() ||
      is_shell_window(window)) {
    return false;
  }
  RECT client{};
  if (!GetClientRect(window, &client)) {
    return false;
  }
  return client.right > client.left && client.bottom > client.top;
}

} // namespace

std::optional<WindowCandidate> describe_attachable_window(HWND window) {
  window = GetAncestor(window, GA_ROOT);
  if (!is_eligible_window(window)) {
    return std::nullopt;
  }

  WindowCandidate candidate;
  candidate.window = window;
  candidate.executable_path =
      process_path_for_window(window, candidate.process_id);
  if (candidate.executable_path.empty()) {
    return std::nullopt;
  }
  candidate.executable_name =
      std::filesystem::path(candidate.executable_path).filename().wstring();
  candidate.title = window_text(window);
  candidate.window_class = window_class(window);

  RECT client{};
  GetClientRect(window, &client);
  candidate.client_area =
      static_cast<std::uint64_t>(client.right - client.left) *
      static_cast<std::uint64_t>(client.bottom - client.top);
  return candidate;
}

std::vector<WindowCandidate> enumerate_attachable_windows() {
  std::vector<WindowCandidate> result;
  EnumWindows(
      [](const HWND window, const LPARAM context) -> BOOL {
        auto &output =
            *reinterpret_cast<std::vector<WindowCandidate> *>(context);
        if (auto candidate = describe_attachable_window(window)) {
          output.push_back(std::move(*candidate));
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&result));
  std::ranges::sort(
      result, [](const WindowCandidate &left, const WindowCandidate &right) {
        if (left.executable_name != right.executable_name) {
          return lower_copy(left.executable_name) <
                 lower_copy(right.executable_name);
        }
        return lower_copy(left.title) < lower_copy(right.title);
      });
  return result;
}

std::optional<WindowCandidate> window_at_point(const POINT point) {
  return describe_attachable_window(WindowFromPoint(point));
}

bool profile_matches_window(const WindowCandidate &candidate,
                            const layerc::core::GameProfile &profile) {
  if (!profile.enabled || profile.game.global ||
      candidate.executable_path.empty()) {
    return false;
  }

  const auto process_path = lower_copy(candidate.executable_path);
  const auto expected_path =
      lower_copy(utf8_to_wide(profile.game.executable_path));
  if (!expected_path.empty() && process_path != expected_path) {
    return false;
  }

  const auto expected_name =
      lower_copy(utf8_to_wide(profile.game.executable_name));
  if (!expected_name.empty() &&
      lower_copy(candidate.executable_name) != expected_name) {
    return false;
  }

  const auto expected_class =
      lower_copy(utf8_to_wide(profile.game.window_class));
  if (!expected_class.empty() &&
      lower_copy(candidate.window_class) != expected_class) {
    return false;
  }

  const auto expected_title =
      lower_copy(utf8_to_wide(profile.game.window_title));
  return expected_title.empty() ||
         lower_copy(candidate.title).find(expected_title) != std::wstring::npos;
}

} // namespace layerc::platform