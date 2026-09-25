#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/visual.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

#include <windows.h>

namespace layerc::test::platform_live {

[[nodiscard]] core::GameProfile test_profile(const std::string &name);
[[nodiscard]] core::VisualProfile test_visual_profile();
[[nodiscard]] core::AutomationProfile
test_automation_profile(const std::string &profile_id = "store-test");

[[nodiscard]] bool launch_auth_peer(const std::filesystem::path &executable,
                                    const wchar_t *mode,
                                    const std::wstring &pipe_name,
                                    PROCESS_INFORMATION &process);
[[nodiscard]] bool durable_read_contract(
    const std::filesystem::path &directory);
[[nodiscard]] bool completed_visual_gc(std::size_t minimum_deleted,
                                       std::size_t minimum_retained);
[[nodiscard]] bool visual_gc_blocked_by(const std::filesystem::path &path);

} // namespace layerc::test::platform_live
