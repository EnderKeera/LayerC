#pragma once

#include "layerc/core/profile.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/native_theme.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

namespace layerc::editor {

using RoleColors = layerc::platform::NativeThemeColors;
using layerc::platform::enable_native_dark_mode;
using layerc::platform::set_dark_title_bar;

// Dark title bar plus the capture-exclusion policy: the chrome every LayerC
// dialog window applies before its children are laid out.
void apply_dialog_chrome(HWND window, bool capture_exclusion);

struct ProfileCheckboxMetrics {
  int image_size{};
  int inset{};
  int pen_width{};
};

void set_owner_draw_checkbox(HWND control, bool checked);
[[nodiscard]] bool owner_draw_checkbox_checked(HWND control);
[[nodiscard]] ProfileCheckboxMetrics profile_checkbox_metrics(UINT dpi);
[[nodiscard]] COLORREF color_ref(std::uint32_t color);
[[nodiscard]] std::uint32_t rgb_value(COLORREF color);
// Fill or frame a rectangle through a temporary solid brush, or outline it
// with a temporary pen, restoring the DC's selected objects.
void fill_solid_rect(HDC dc, const RECT &rect, COLORREF color);
void frame_solid_rect(HDC dc, const RECT &rect, COLORREF color);
void outline_rect(HDC dc, const RECT &rect, int pen_width, COLORREF color);
[[nodiscard]] bool start_daemon();
[[nodiscard]] bool recycle_profile_bundle(const std::filesystem::path &path);
[[nodiscard]] std::string
safe_identifier(std::wstring_view text, std::string_view fallback = "profile");
[[nodiscard]] layerc::core::GameProfile make_new_profile();
[[nodiscard]] std::wstring
profile_list_label(const layerc::core::GameProfile &profile);

// Smallest "<base>" then "<base>-<n>" id unused by the predicate.
template <typename IsUsed>
[[nodiscard]] std::string
next_unique_id(const std::string_view base, IsUsed &&is_used) {
  std::string candidate(base);
  for (std::size_t suffix = 2; is_used(candidate); ++suffix)
    candidate = std::string(base) + "-" + std::to_string(suffix);
  return candidate;
}

[[nodiscard]] std::string
unique_panel_id(std::string_view base,
                const std::vector<layerc::core::Panel> &existing,
                const std::vector<layerc::core::Panel> *generated = nullptr);
[[nodiscard]] std::wstring
profile_error_text(const layerc::core::ProfileError &error);
[[nodiscard]] bool confirm_schema_upgrade(HWND owner);
void erase_visual_assignment(layerc::core::VisualProfile &visual,
                             std::string_view cooldown_id);
void gc_orphan_visual_observation(layerc::core::VisualProfile &visual,
                                  std::string_view observation_id);

} // namespace layerc::editor
