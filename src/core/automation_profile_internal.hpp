#pragma once

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

} // namespace layerc::core::detail
