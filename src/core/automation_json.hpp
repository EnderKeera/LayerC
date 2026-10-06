#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/json_reader.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::core::detail {

using json::max_exact_json_integer;
using json::append_exact_integer;
using json::append_exact_unsigned;

[[nodiscard]] inline std::optional<std::int64_t>
read_exact_integer(const json::Value &value) {
  return json::read_integer(value, json::IntegerEncoding::number_or_string);
}
[[nodiscard]] inline std::optional<std::uint64_t>
read_exact_unsigned(const json::Value &value) {
  return json::read_unsigned(value, json::IntegerEncoding::number_or_string);
}

// Automation sidecars, values, state, and traces parse through
// json::parse_bounded, which reports syntax errors at "$@<byte offset>".
[[nodiscard]] inline AutomationError
json_parse_error(const json::FieldError &error) {
  return error;
}

// The wire-name vocabulary lives in the public automation.hpp; these
// aliases keep the detail:: call sites in the serializer and runtime working.
using layerc::core::automation_event_kinds;
using layerc::core::automation_event_name;
using layerc::core::automation_value_type_name;
using layerc::core::parse_automation_event_name;
using layerc::core::parse_automation_value_type_name;


} // namespace layerc::core::detail
