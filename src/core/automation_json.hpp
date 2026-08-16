#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/json.hpp"

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

// Canonical enum<->wire-name tables shared by the profile serializer, the
// runtime trace format, and their parsers. The two formats deliberately use
// identical names so traces replay through the profile reader.
[[nodiscard]] inline std::string_view
automation_event_name(const AutomationEventKind kind) {
  switch (kind) {
  case AutomationEventKind::input_pressed:
    return "inputPressed";
  case AutomationEventKind::input_released:
    return "inputReleased";
  case AutomationEventKind::input_value_changed:
    return "inputValueChanged";
  case AutomationEventKind::visual_observation:
    return "visualObservation";
  case AutomationEventKind::cooldown_triggered:
    return "cooldownTriggered";
  case AutomationEventKind::cooldown_ready:
    return "cooldownReady";
  case AutomationEventKind::group_triggered:
    return "groupTriggered";
  case AutomationEventKind::group_ready:
    return "groupReady";
  case AutomationEventKind::session_started:
    return "sessionStarted";
  case AutomationEventKind::session_stopping:
    return "sessionStopping";
  case AutomationEventKind::variable_changed:
    return "variableChanged";
  case AutomationEventKind::timer_fired:
    return "timerFired";
  }
  return "sessionStarted";
}

[[nodiscard]] inline std::optional<AutomationEventKind>
parse_automation_event_name(const std::string_view name) {
  for (const auto kind :
       {AutomationEventKind::input_pressed, AutomationEventKind::input_released,
        AutomationEventKind::input_value_changed,
        AutomationEventKind::visual_observation,
        AutomationEventKind::cooldown_triggered,
        AutomationEventKind::cooldown_ready,
        AutomationEventKind::group_triggered, AutomationEventKind::group_ready,
        AutomationEventKind::session_started,
        AutomationEventKind::session_stopping,
        AutomationEventKind::variable_changed,
        AutomationEventKind::timer_fired})
    if (automation_event_name(kind) == name)
      return kind;
  return std::nullopt;
}

[[nodiscard]] inline std::string_view
automation_value_type_name(const AutomationValueType type) {
  switch (type) {
  case AutomationValueType::null_value:
    return "null";
  case AutomationValueType::boolean:
    return "bool";
  case AutomationValueType::integer:
    return "int";
  case AutomationValueType::number:
    return "number";
  case AutomationValueType::duration:
    return "duration";
  case AutomationValueType::confidence:
    return "confidence";
  case AutomationValueType::string:
    return "string";
  case AutomationValueType::list:
    return "list";
  case AutomationValueType::map:
    return "map";
  }
  return "null";
}

} // namespace layerc::core::detail