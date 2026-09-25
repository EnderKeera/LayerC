#pragma once

#include "layerc/platform/profile_store.hpp"

namespace layerc::platform {

// Linux XDG store topology. Empty or relative overrides use the HOME fallback;
// an empty or relative HOME yields empty roots.
[[nodiscard]] ProfileStoreRoots profile_store_roots();
[[nodiscard]] ProfileStoreRoots profile_store_roots_from(
    const std::filesystem::path &home,
    const std::filesystem::path &config_home = {},
    const std::filesystem::path &data_home = {});
[[nodiscard]] bool ensure_profile_store_roots(const ProfileStoreRoots &roots);

} // namespace layerc::platform
