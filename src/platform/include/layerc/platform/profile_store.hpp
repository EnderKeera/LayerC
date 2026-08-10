#pragma once

#include "layerc/core/visual.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::platform {

inline constexpr wchar_t profile_reload_event_name[] =
    L"Local\\LayerC.LayerD.ProfileReload";

[[nodiscard]] std::filesystem::path profile_directory();
[[nodiscard]] std::filesystem::path visual_directory();
[[nodiscard]] std::filesystem::path
visual_sidecar_path(const std::filesystem::path &profile_path);
[[nodiscard]] std::wstring
canonical_profile_key(const std::filesystem::path &path);
[[nodiscard]] std::optional<std::string>
read_profile_file(const std::filesystem::path &path);
[[nodiscard]] bool write_profile_file(const std::filesystem::path &path,
                                      std::string_view contents);
// Commits the base profile and its optional visual sidecar as a consistent
// bundle. When both files are present, the replacement order and rollback
// preserve either the previous valid pair or the new pair if either write
// fails partway through.
[[nodiscard]] bool write_profile_bundle(
    const std::filesystem::path &path,
    const layerc::core::GameProfile &profile,
    const std::optional<layerc::core::VisualProfile> &visual_profile);

enum class VisualSidecarState {
  absent,
  valid,
  recovered_backup,
  invalid,
};

struct VisualSidecarLoad {
  VisualSidecarState state{VisualSidecarState::absent};
  std::optional<layerc::core::VisualProfile> profile;
  layerc::core::VisualError error;
  std::filesystem::path source_path;
};

[[nodiscard]] VisualSidecarLoad load_visual_sidecar(
    const std::filesystem::path &profile_path,
    const layerc::core::GameProfile &base_profile);
[[nodiscard]] bool write_visual_sidecar(
    const std::filesystem::path &profile_path,
    const layerc::core::VisualProfile &visual_profile,
    const layerc::core::GameProfile &base_profile);
[[nodiscard]] bool signal_profile_reload();

} // namespace layerc::platform
