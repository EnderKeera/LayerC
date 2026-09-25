#pragma once

#include "layerc/platform/profile_store.hpp"

namespace layerc::platform {

// Platform-specific roots and final-path lookup; common layout and identity
// rules live in profile_store_layout.cpp.
[[nodiscard]] ProfileStoreRoots
profile_layout_roots(const std::filesystem::path &profile_path);
[[nodiscard]] std::filesystem::path
profile_layout_final_path(const std::filesystem::path &path);

} // namespace layerc::platform
