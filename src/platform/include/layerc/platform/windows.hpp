#pragma once

#include "layerc/core/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace layerc::platform {

inline constexpr wchar_t settings_reload_event_name[] =
    L"Local\\LayerC.LayerD.SettingsReload";

enum class EditorTheme : std::uint32_t {
  copper = 0,
  teal = 1,
  plum = 2,
};

// These roles intentionally map to native control families rather than a
// particular window instance. LayerE's appearance settings are global.
enum class EditorControlRole : std::uint32_t {
  window = 0,
  toolbar = 1,
  sidebar = 2,
  label = 3,
  button = 4,
  edit = 5,
  list = 6,
  checkbox = 7,
  preview = 8,
  status = 9,
  count,
};

enum EditorAppearanceField : std::uint32_t {
  editor_fill = 1U << 0U,
  editor_text = 1U << 1U,
  editor_border = 1U << 2U,
  editor_hover = 1U << 3U,
  editor_selection = 1U << 4U,
  editor_disabled = 1U << 5U,
};

struct EditorAppearanceOverrides {
  std::uint32_t fields{};
  std::uint32_t fill{};
  std::uint32_t text{};
  std::uint32_t border{};
  std::uint32_t hover{};
  std::uint32_t selection{};
  std::uint32_t disabled{};
};

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
[[nodiscard]] bool
profile_matches_window(const WindowCandidate &candidate,
                       const layerc::core::GameProfile &profile);

[[nodiscard]] bool capture_exclusion_enabled();
[[nodiscard]] bool set_capture_exclusion_enabled(bool enabled);
[[nodiscard]] bool apply_capture_exclusion(HWND window, bool enabled);
[[nodiscard]] EditorTheme editor_theme();
[[nodiscard]] std::uint32_t editor_accent();
[[nodiscard]] bool set_editor_theme(EditorTheme theme);
[[nodiscard]] bool set_editor_accent(std::uint32_t color);
[[nodiscard]] EditorAppearanceOverrides
editor_appearance_overrides(EditorControlRole role);
[[nodiscard]] bool set_editor_appearance_overrides(
    EditorControlRole role, const EditorAppearanceOverrides &overrides);
[[nodiscard]] bool reset_editor_appearance_overrides(EditorControlRole role,
                                                      std::uint32_t fields);
[[nodiscard]] bool signal_settings_reload();

} // namespace layerc::platform