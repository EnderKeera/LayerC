#pragma once

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <variant>

namespace layerc::platform {

// A base profile plus its module sidecars, loaded through the same readers the
// daemon uses. Legacy flags derive from the core schema constants so callers
// never compare schema literals themselves.
struct ProfileBundleLoad {
  layerc::core::GameProfile profile;
  ProfileModuleLoads modules;
  bool legacy_base_schema{};
  bool legacy_visual_schema{};
};

enum class ProfileBundleLoadError { unreadable, invalid_profile };

[[nodiscard]] std::variant<ProfileBundleLoad, ProfileBundleLoadError>
load_profile_bundle(const std::filesystem::path &path);

// True when `source` is a JSON object whose numeric schemaVersion is older
// than `current_schema`; malformed input is not treated as legacy.
[[nodiscard]] bool source_schema_before(std::string_view source,
                                        std::uint32_t current_schema);

} // namespace layerc::platform
