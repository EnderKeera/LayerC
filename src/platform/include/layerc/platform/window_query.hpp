#pragma once

#include "layerc/core/types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace layerc::platform {

struct WindowCandidate {
  HWND window{};
  DWORD process_id{};
  std::wstring title;
  std::wstring executable_path;
  std::wstring executable_name;
  std::wstring window_class;
  std::uint64_t client_area{};
};

[[nodiscard]] std::vector<WindowCandidate> enumerate_attachable_windows();
[[nodiscard]] std::optional<WindowCandidate>
describe_attachable_window(HWND window);
[[nodiscard]] std::optional<WindowCandidate> window_at_point(POINT point);
// `filename` next to the current executable; empty if the module path is
// unavailable.
[[nodiscard]] std::filesystem::path sibling_image_path(const wchar_t *filename);
// True when the process image path is equivalent to `expected`.
[[nodiscard]] bool process_image_equals(HANDLE process,
                                        const std::filesystem::path &expected);
[[nodiscard]] bool is_sibling_process_image(DWORD process_id,
                                             const wchar_t *filename);
[[nodiscard]] bool same_user_as_current_process(HANDLE process);
[[nodiscard]] bool
profile_matches_window(const WindowCandidate &candidate,
                       const layerc::core::GameProfile &profile);
[[nodiscard]] bool apply_capture_exclusion(HWND window, bool enabled);
[[nodiscard]] UINT monitor_dpi_for_rect(const RECT &rect);
[[nodiscard]] bool is_gamebar_overlay_process(DWORD process_id);
[[nodiscard]] std::string window_session_key(HWND window);

} // namespace layerc::platform
