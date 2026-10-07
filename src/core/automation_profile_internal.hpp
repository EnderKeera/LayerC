#pragma once

#include "layerc/core/automation.hpp"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string_view>

namespace layerc::core::detail {

inline bool valid_automation_id(const std::string_view value) {
  return !value.empty() && value.size() <= 128 &&
         std::ranges::all_of(value, [](const unsigned char character) {
           return std::isalnum(character) || character == '-' ||
                  character == '_' || character == '.';
         });
}

// The automation document header rule: schema 1, and (when `base` is given)
// the base profile's ID. The parser applies only the schema rule (no `base`)
// before reading the body so a different schema reports its version;
// validate_automation_profile applies the complete rule to every profile.
[[nodiscard]] inline AutomationError
automation_header_error(const AutomationProfile &profile,
                        const GameProfile *base) {
  if (profile.schema_version != 1)
    return {"$.schemaVersion", "unsupported automation schema version"};
  if (base != nullptr && profile.profile_id != base->id)
    return {"$.profileId", "automation profile ID does not match base profile"};
  return {};
}

} // namespace layerc::core::detail
