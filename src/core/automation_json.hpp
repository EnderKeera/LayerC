#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/json_reader.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::core::detail {

[[nodiscard]] inline std::optional<std::int64_t>
read_exact_integer(const json::Value &value) {
  return json::read_integer(value, json::IntegerEncoding::number_or_string);
}
[[nodiscard]] inline std::optional<std::uint64_t>
read_exact_unsigned(const json::Value &value) {
  return json::read_unsigned(value, json::IntegerEncoding::number_or_string);
}

// The same bounds as automation_value_valid, with an absolute nesting depth.
// The codec uses this only to locate the failing child after validation fails.
[[nodiscard]] bool automation_value_valid_at_depth(
    const AutomationValue &value, std::size_t depth) noexcept;

// The one AutomationValue codec (automation_value_codec.cpp). A value whose
// type the reader already knows is bare JSON; list and map children, and
// values of unknown type, are {type-tag, value-tag} objects. Profile
// literals and persisted state tag with $type/$value, traces with
// type/value. Every bound comes from automation_value_valid.
struct AutomationValueTags {
  std::string_view type;
  std::string_view value;
};
inline constexpr AutomationValueTags state_value_tags{"$type", "$value"};
inline constexpr AutomationValueTags trace_value_tags{"type", "value"};

[[nodiscard]] json::Value
automation_value_tree(const AutomationValue &value,
                      AutomationValueTags tags = state_value_tags);
[[nodiscard]] json::Value
tagged_automation_value_tree(const AutomationValue &value, AutomationValueTags tags);

void append_automation_value(std::string &out, const AutomationValue &value,
                             AutomationValueTags tags = state_value_tags);
void append_tagged_automation_value(std::string &out,
                                    const AutomationValue &value,
                                    AutomationValueTags tags);
[[nodiscard]] std::optional<AutomationValue>
read_automation_value(const json::Value &value, AutomationValueType type,
                      AutomationValueTags tags, AutomationError &error,
                      const std::string &path);
[[nodiscard]] std::optional<AutomationValue>
read_tagged_automation_value(const json::Value &value,
                             AutomationValueTags tags, AutomationError &error,
                             const std::string &path);

} // namespace layerc::core::detail
