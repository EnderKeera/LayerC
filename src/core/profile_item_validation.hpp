#pragma once

#include "layerc/core/profile.hpp"

namespace layerc::core::detail {

[[nodiscard]] std::optional<ProfileError>
validate_profile_items(const GameProfile &profile);

} // namespace layerc::core::detail
