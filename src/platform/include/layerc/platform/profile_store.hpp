#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::platform {

inline constexpr wchar_t profile_reload_event_name[] =
    L"Local\\LayerC.LayerD.ProfileReload";

[[nodiscard]] std::filesystem::path profile_directory();
[[nodiscard]] std::optional<std::string>
read_profile_file(const std::filesystem::path &path);
[[nodiscard]] bool write_profile_file(const std::filesystem::path &path,
                                      std::string_view contents);
[[nodiscard]] bool signal_profile_reload();

} // namespace layerc::platform