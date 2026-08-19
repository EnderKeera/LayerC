#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/json.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::core::detail {

inline constexpr std::int64_t max_exact_json_integer =
    INT64_C(9'007'199'254'740'991);

[[nodiscard]] inline std::optional<std::int64_t>
read_exact_integer(const json::Value &value) {
  if (value.is_integer())
    return value.as_integer();
  if (value.is_string()) {
    std::int64_t result{};
    const auto &source = value.as_string();
    const auto conversion =
        std::from_chars(source.data(), source.data() + source.size(), result);
    if (conversion.ec == std::errc{} &&
        conversion.ptr == source.data() + source.size())
      return result;
    return std::nullopt;
  }
  if (!value.is_number() || !std::isfinite(value.as_number()) ||
      std::floor(value.as_number()) != value.as_number() ||
      value.as_number() < -static_cast<double>(max_exact_json_integer) ||
      value.as_number() > static_cast<double>(max_exact_json_integer))
    return std::nullopt;
  return static_cast<std::int64_t>(value.as_number());
}

[[nodiscard]] inline std::optional<std::uint64_t>
read_exact_unsigned(const json::Value &value) {
  if (value.is_integer()) {
    const auto integer = value.as_integer();
    if (integer >= 0)
      return static_cast<std::uint64_t>(integer);
    return std::nullopt;
  }
  if (value.is_string()) {
    std::uint64_t result{};
    const auto &source = value.as_string();
    const auto conversion =
        std::from_chars(source.data(), source.data() + source.size(), result);
    if (conversion.ec == std::errc{} &&
        conversion.ptr == source.data() + source.size())
      return result;
    return std::nullopt;
  }
  if (!value.is_number() || !std::isfinite(value.as_number()) ||
      std::floor(value.as_number()) != value.as_number() ||
      value.as_number() < 0.0 ||
      value.as_number() > static_cast<double>(max_exact_json_integer))
    return std::nullopt;
  return static_cast<std::uint64_t>(value.as_number());
}

inline void append_exact_unsigned(std::string &output,
                                  const std::uint64_t value) {
  if (value <= static_cast<std::uint64_t>(max_exact_json_integer))
    json::append_unsigned(output, value);
  else
    json::append_quoted(output, std::to_string(value));
}
inline void append_exact_integer(std::string &output,
                                 const std::int64_t value) {
  if (value >= -max_exact_json_integer && value <= max_exact_json_integer) {
    if (value < 0) {
      output.push_back('-');
      json::append_unsigned(output,
                            static_cast<std::uint64_t>(-(value + 1)) + 1U);
    } else {
      json::append_unsigned(output, static_cast<std::uint64_t>(value));
    }
    return;
  }
  json::append_quoted(output, std::to_string(value));
}

// The wire-name vocabulary lives in the public automation.hpp; these
// aliases keep the detail:: call sites in the serializer and runtime working.
using layerc::core::automation_event_kinds;
using layerc::core::automation_event_name;
using layerc::core::automation_value_type_name;
using layerc::core::parse_automation_event_name;
using layerc::core::parse_automation_value_type_name;


} // namespace layerc::core::detail