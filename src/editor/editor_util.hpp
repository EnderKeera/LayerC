#pragma once

#include "layerc/core/profile.hpp"
#include "layerc/core/visual.hpp"
#include "editor_prompt.hpp"
#include "profile_edit.hpp"
#include "layerc/platform/native_theme.hpp"

#include <array>
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
using layerc::platform::fill_solid_rect;
using layerc::platform::frame_solid_rect;
using layerc::platform::native_color_ref;
using layerc::platform::native_rgb_value;
using layerc::platform::native_window_dpi;
using layerc::platform::outline_rect;
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
// Options > Text size, stored as a DWORD percent under HKCU\Software\LayerC.
inline constexpr std::array text_scale_choices{100, 115, 130};
inline constexpr std::wstring_view text_scale_setting = L"EditorTextScale";
[[nodiscard]] int stored_text_scale();

[[nodiscard]] ProfileCheckboxMetrics profile_checkbox_metrics(UINT dpi);
// True when `point` (list client coordinates) lies on the enablement
// checkbox of the profile row whose bounds are `row`.
[[nodiscard]] bool profile_checkbox_hit(const RECT &row, POINT point,
                                        UINT dpi);
[[nodiscard]] bool start_daemon();
[[nodiscard]] bool recycle_profile_bundle(const std::filesystem::path &path);
[[nodiscard]] std::wstring
profile_list_label(const layerc::core::GameProfile &profile);

[[nodiscard]] std::wstring
profile_error_text(const layerc::core::ProfileError &error);
[[nodiscard]] bool confirm_schema_upgrade(HWND owner, bool items = false);

} // namespace layerc::editor
