#include "layerc/platform/window_query.hpp"

#include "layerc/core/game_match.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <vector>

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

  std::vector<wchar_t> path(32'768);
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
  return klass == L"Shell_TrayWnd" || klass == L"Shell_SecondaryTrayWnd" ||
         klass == L"Progman" ||
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
  // Case-insensitive ordering with the sort keys lower-cased once per
  // window instead of twice per comparison.
  struct SortKey {
    std::wstring executable;
    std::wstring title;
    std::size_t index;
  };
  std::vector<SortKey> order;
  order.reserve(result.size());
  for (std::size_t index = 0; index < result.size(); ++index)
    order.push_back({lower_copy(result[index].executable_name),
                     lower_copy(result[index].title), index});
  std::ranges::sort(order, [](const SortKey &left, const SortKey &right) {
    if (left.executable != right.executable)
      return left.executable < right.executable;
    return left.title < right.title;
  });
  std::vector<WindowCandidate> sorted;
  sorted.reserve(result.size());
  for (const auto &entry : order)
    sorted.push_back(std::move(result[entry.index]));
  return sorted;
}

std::optional<WindowCandidate> window_at_point(const POINT point) {
  return describe_attachable_window(WindowFromPoint(point));
}

bool profile_matches_window(const WindowCandidate &candidate,
                            const layerc::core::GameProfile &profile) {
  if (!profile.enabled) return false;
  // Windows filesystems compare case-insensitively: the shared policy folds
  // the executable path (exact_path=false). Enumeration stays here; the
  // matching rules live in core::candidate_matches_game.
  const auto executable_path = try_wide_to_utf8(candidate.executable_path);
  const auto executable_name = try_wide_to_utf8(candidate.executable_name);
  const auto window_class = try_wide_to_utf8(candidate.window_class);
  const auto title = try_wide_to_utf8(candidate.title);
  if (!executable_path.has_value() || !executable_name.has_value() ||
      !window_class.has_value() || !title.has_value())
    return false;
  const layerc::core::MatchCandidate fields{
      .executable_name = *executable_name,
      .executable_path = *executable_path,
      .class_name = *window_class,
      .title = *title,
      .active = false};
  return layerc::core::candidate_matches_game(fields, profile.game,
                                               /*exact_path=*/false);
}

} // namespace layerc::platform
