#pragma once

#include "layerc/core/types.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace layerc::core {

inline constexpr std::size_t max_profile_json_bytes = 256U * 1024U;

struct ProfileError {
  std::string path;
  std::string message;
};

using ProfileResult = std::variant<GameProfile, ProfileError>;

[[nodiscard]] ProfileResult parse_profile_json(std::string_view source);
[[nodiscard]] std::string serialize_profile_json(const GameProfile &profile);

} // namespace layerc::core
