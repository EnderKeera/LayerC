#pragma once

// Operator lookup exposes shape, static inference and evaluation; the registry
// and implementations are private to automation_ops.cpp.
#include "layerc/core/automation.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace layerc::core::detail {

[[nodiscard]] bool truthy(const AutomationValue &value);
[[nodiscard]] std::optional<double> number(const AutomationValue &value);
[[nodiscard]] std::optional<AutomationValue> duration_result(double value);

enum class AutomationOpcode : std::uint8_t {
  logical_not, negate, size, add, subtract, multiply, divide, modulo,
  equal, not_equal, less, less_equal, greater, greater_equal,
  logical_and, logical_or, minimum, maximum, clamp, any, all, map, filter,
};

[[nodiscard]] std::optional<std::int64_t>
checked_integer_binary(std::int64_t left, std::int64_t right,
                       AutomationOpcode operation);
[[nodiscard]] bool numeric_type(AutomationValueType type);

using AutomationOpTypeResult =
    std::variant<std::optional<AutomationValueType>, AutomationError>;
using AutomationOpResult = std::variant<AutomationValue, AutomationError>;

struct AutomationOpSpec {
  std::string_view name;
  AutomationOpcode opcode;
  ExpressionKind kind;
  std::uint8_t min_args;
  std::uint8_t max_args;
  AutomationOpTypeResult (*infer)(
      const AutomationOpSpec &self,
      std::span<const std::optional<AutomationValueType>> argument_types);
  AutomationOpResult (*evaluate)(const AutomationOpSpec &self,
                                 std::span<const AutomationValue> arguments);
};

[[nodiscard]] const AutomationOpSpec *
find_automation_op(std::string_view name) noexcept;

} // namespace layerc::core::detail
