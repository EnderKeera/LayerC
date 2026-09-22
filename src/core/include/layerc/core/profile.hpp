#pragma once

#include "layerc/core/types.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace layerc::core {
namespace json { class Value; }

inline constexpr std::size_t max_profile_json_bytes = 256U * 1024U;
inline constexpr std::uint32_t profile_schema_version = 4;

struct ProfileError {
  std::string path;
  std::string message;
};

using ProfileResult = std::variant<GameProfile, ProfileError>;

[[nodiscard]] ProfileResult parse_profile_json(std::string_view source);
// For a bounded document already accepted by json::parse (including UTF-8).
[[nodiscard]] ProfileResult parse_profile_value(const json::Value &root);
[[nodiscard]] std::string serialize_profile_json(const GameProfile &profile);

} // namespace layerc::core
