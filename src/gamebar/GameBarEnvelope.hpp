#pragma once

// App Service ValueSet envelope shared by the desktop client
// (GameBarAppServiceClient.cpp) and the widget (widget/WidgetSnapshotHub.cpp).
// Both ends ship in one MSIX, so the keys only need to agree within a build.
namespace layerc::gamebar::envelope {

// Request keys.
inline constexpr wchar_t kind_key[] = L"kind";
inline constexpr wchar_t json_key[] = L"json";

// Response keys.
inline constexpr wchar_t accepted_key[] = L"accepted";
inline constexpr wchar_t error_key[] = L"error";

// Values of kind_key.
inline constexpr wchar_t snapshot_kind[] = L"snapshot";
inline constexpr wchar_t preferences_kind[] = L"preferences";

} // namespace layerc::gamebar::envelope
