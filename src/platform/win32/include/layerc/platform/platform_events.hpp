#pragma once

namespace layerc::platform {

inline constexpr wchar_t profile_reload_event_name[] =
    L"Local\\LayerC.LayerD.ProfileReload";
inline constexpr wchar_t settings_reload_event_name[] =
    L"Local\\LayerC.LayerD.SettingsReload";
inline constexpr wchar_t visual_capture_active_event_name[] =
    L"Local\\LayerC.LayerD.VisualCaptureActive";
inline constexpr wchar_t automation_active_event_name[] =
    L"Local\\LayerC.LayerD.AutomationActive.v1";

// Opens and sets an existing named event; false if it is absent or not set.
[[nodiscard]] bool signal_named_event(const wchar_t *name);
[[nodiscard]] bool signal_settings_reload();

} // namespace layerc::platform
