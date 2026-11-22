#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

namespace layerc::editor {

[[nodiscard]] std::optional<std::string>
recover_startup_profiles(const std::filesystem::path &directory);

[[nodiscard]] int run_windows_editor(HINSTANCE instance,
                                     std::wstring_view command_line);

} // namespace layerc::editor
