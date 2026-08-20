#pragma once

// One registry for expression operators: names, expression shape, static type
// inference, and evaluation all live here so the parser, the validator, and
// the runtime cannot drift apart. Structural expressions (literal, variable,
// event field, index) have distinct JSON shapes and stay hand-written at
// their call sites.

#include "layerc/core/automation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace layerc::core::detail {

[[nodiscard]] inline bool truthy(const AutomationValue &value) {
  if (const auto *v = std::get_if<bool>(&value.storage))
    return *v;
  if (const auto *v = std::get_if<std::int64_t>(&value.storage))
    return *v != 0;
  if (const auto *v = std::get_if<double>(&value.storage))
    return *v != 0;
  if (const auto *v = std::get_if<DurationValue>(&value.storage))
    return v->value != Milliseconds::zero();
  if (const auto *v = std::get_if<ConfidenceValue>(&value.storage))
    return v->value != 0;
  if (const auto *v = std::get_if<std::string>(&value.storage))
    return !v->empty();
  if (const auto *v = std::get_if<AutomationList>(&value.storage))
    return !v->empty();
  if (const auto *v = std::get_if<AutomationMap>(&value.storage))
    return !v->empty();
  return false;
}

[[nodiscard]] inline std::optional<double> number(const AutomationValue &value) {
  if (const auto *v = std::get_if<std::int64_t>(&value.storage))
    return static_cast<double>(*v);
  if (const auto *v = std::get_if<double>(&value.storage))
    return *v;
  if (const auto *v = std::get_if<DurationValue>(&value.storage))
    return static_cast<double>(v->value.count());
  if (const auto *v = std::get_if<ConfidenceValue>(&value.storage))
    return v->value;
  return std::nullopt;
}

[[nodiscard]] inline std::optional<AutomationValue>
duration_result(const double value) {
  if (!std::isfinite(value) || value < 0.0 || value > 86'400'000.0 ||
      std::floor(value) != value)
    return std::nullopt;
  return AutomationValue(
      DurationValue{Milliseconds(static_cast<std::int64_t>(value))});
}

[[nodiscard]] inline std::optional<std::int64_t>
checked_integer_binary(const std::int64_t left, const std::int64_t right,
                       const std::string_view operation) {
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  if (operation == "add") {
    if ((right > 0 && left > maximum - right) ||
        (right < 0 && left < minimum - right))
      return std::nullopt;
    return left + right;
  }
  if (operation == "subtract") {
    if ((right < 0 && left > maximum + right) ||
        (right > 0 && left < minimum + right))
      return std::nullopt;
    return left - right;
  }
  if (operation == "multiply") {
    if (left == 0 || right == 0)
      return 0;
    if ((left == -1 && right == minimum) || (right == -1 && left == minimum))
      return std::nullopt;
    if (left > 0) {
      if ((right > 0 && left > maximum / right) ||
          (right < 0 && right < minimum / left))
        return std::nullopt;
    } else {
      if ((right > 0 && left < minimum / right) ||
          (right < 0 && left < maximum / right))
        return std::nullopt;
    }
    return left * right;
  }
  if (operation == "modulo") {
    if (right == 0 || (left == minimum && right == -1))
      return std::nullopt;
    return left % right;
  }
  return std::nullopt;
}

[[nodiscard]] inline bool
numeric_type(const AutomationValueType type) {
  return type == AutomationValueType::integer ||
         type == AutomationValueType::number ||
         type == AutomationValueType::duration ||
         type == AutomationValueType::confidence;
}

struct AutomationOpSpec {
  std::string_view name;
  ExpressionKind kind;
  std::uint8_t min_args;
  std::uint8_t max_args;
  // Static inference over the argument types (nullopt = dynamically typed).
  // The result bool is checked by the evaluator; [[nodiscard]] cannot be
  // applied to function-pointer members (MSVC C4869 ignores it).
  bool (*infer)(
      const AutomationOpSpec &self,
      std::span<const std::optional<AutomationValueType>> argument_types,
      std::optional<AutomationValueType> &result, std::string &error);
  // Dynamic evaluation over already-evaluated arguments. Callers check the
  // bool; see the note above about [[nodiscard]] on function-pointer members.
  bool (*evaluate)(const AutomationOpSpec &self,
                   std::span<const AutomationValue> arguments,
                   AutomationValue &result, std::string &error);
};

namespace op_impl {

using Args = std::span<const AutomationValue>;
using Types = std::span<const std::optional<AutomationValueType>>;

// --- inference ---

inline bool known(const Types types, const std::size_t index,
                  const AutomationValueType expected) {
  return !types[index].has_value() || *types[index] == expected;
}

inline bool numeric(const Types types, const std::size_t index) {
  return !types[index].has_value() || numeric_type(*types[index]);
}

inline bool infer_boolean_unary(const AutomationOpSpec &self, const Types types,
                                std::optional<AutomationValueType> &result,
                                std::string &error) {
  if (!known(types, 0, AutomationValueType::boolean)) {
    error = std::string(self.name) + " requires a Boolean";
    return false;
  }
  result = AutomationValueType::boolean;
  return true;
}

inline bool infer_negate(const AutomationOpSpec &, const Types types,
                         std::optional<AutomationValueType> &result,
                         std::string &error) {
  if (types[0].has_value() && *types[0] != AutomationValueType::integer &&
      *types[0] != AutomationValueType::number) {
    error = "negate requires an integer or number";
    return false;
  }
  result = types[0];
  return true;
}

inline bool infer_size(const AutomationOpSpec &, const Types types,
                       std::optional<AutomationValueType> &result,
                       std::string &error) {
  if (types[0].has_value() && *types[0] != AutomationValueType::string &&
      *types[0] != AutomationValueType::list &&
      *types[0] != AutomationValueType::map) {
    error = "size requires a string, list, or map";
    return false;
  }
  result = AutomationValueType::integer;
  return true;
}

inline bool infer_boolean_binary(const AutomationOpSpec &self, const Types types,
                                 std::optional<AutomationValueType> &result,
                                 std::string &error) {
  if (!known(types, 0, AutomationValueType::boolean) ||
      !known(types, 1, AutomationValueType::boolean)) {
    error = std::string(self.name) + " requires Booleans";
    return false;
  }
  result = AutomationValueType::boolean;
  return true;
}

inline bool infer_equality(const AutomationOpSpec &, const Types types,
                           std::optional<AutomationValueType> &result,
                           std::string &error) {
  if (types[0].has_value() && types[1].has_value() &&
      *types[0] != *types[1]) {
    error = "equality operands must have the same type";
    return false;
  }
  result = AutomationValueType::boolean;
  return true;
}

inline bool infer_comparison(const AutomationOpSpec &self, const Types types,
                             std::optional<AutomationValueType> &result,
                             std::string &error) {
  if (!numeric(types, 0) || !numeric(types, 1)) {
    error = std::string(self.name) + " requires numeric operands";
    return false;
  }
  result = AutomationValueType::boolean;
  return true;
}

inline bool infer_arithmetic(const AutomationOpSpec &self, const Types types,
                             std::optional<AutomationValueType> &result,
                             std::string &error) {
  if (!numeric(types, 0) || !numeric(types, 1)) {
    error = std::string(self.name) + " requires numeric operands";
    return false;
  }
  const bool left_duration = types[0] == AutomationValueType::duration;
  const bool right_duration = types[1] == AutomationValueType::duration;
  if ((left_duration || right_duration) &&
      (!left_duration || !right_duration ||
       (self.name != "add" && self.name != "subtract"))) {
    error = "durations only support add and subtract";
    return false;
  }
  if (self.name != "divide" &&
      types[0] == AutomationValueType::integer &&
      types[1] == AutomationValueType::integer)
    result = AutomationValueType::integer;
  else if (left_duration && right_duration)
    result = AutomationValueType::duration;
  else
    result = AutomationValueType::number;
  return true;
}

inline bool infer_any_all(const AutomationOpSpec &self, const Types types,
                          std::optional<AutomationValueType> &result,
                          std::string &error) {
  if (types.size() != 1 ||
      (types[0].has_value() && *types[0] != AutomationValueType::list)) {
    error = std::string(self.name) + " requires one list";
    return false;
  }
  result = AutomationValueType::boolean;
  return true;
}

inline bool infer_map_filter(const AutomationOpSpec &self, const Types types,
                             std::optional<AutomationValueType> &result,
                             std::string &error) {
  if (types.size() != 2 ||
      (types[0].has_value() && *types[0] != AutomationValueType::list) ||
      !known(types, 1, AutomationValueType::string)) {
    error = std::string(self.name) + " requires a list and a string field name";
    return false;
  }
  result = AutomationValueType::list;
  return true;
}

inline bool infer_min_max_clamp(const AutomationOpSpec &self, const Types types,
                                std::optional<AutomationValueType> &result,
                                std::string &error) {
  const bool valid_count = (self.name == "clamp" && types.size() == 3U) ||
                           ((self.name == "min" || self.name == "max") &&
                            !types.empty() && types.size() <= 3U);
  if (!valid_count ||
      !std::ranges::all_of(
          std::views::iota(std::size_t{}, types.size()),
          [&](const std::size_t index) { return numeric(types, index); })) {
    error = std::string(self.name) + " requires bounded numeric arguments";
    return false;
  }
  result = AutomationValueType::number;
  return true;
}

// --- evaluation ---

inline bool eval_not(const AutomationOpSpec &, const Args arguments,
                     AutomationValue &result, std::string &) {
  result = AutomationValue(!truthy(arguments[0]));
  return true;
}

inline bool eval_negate(const AutomationOpSpec &, const Args arguments,
                        AutomationValue &result, std::string &error) {
  if (const auto *integer = std::get_if<std::int64_t>(&arguments[0].storage)) {
    if (*integer == std::numeric_limits<std::int64_t>::min()) {
      error = "integer negation overflow";
      return false;
    }
    result = AutomationValue(-*integer);
    return true;
  }
  if (const auto *value = std::get_if<double>(&arguments[0].storage)) {
    if (!std::isfinite(-*value)) {
      error = "numeric result is not finite";
      return false;
    }
    result = AutomationValue(-*value);
    return true;
  }
  error = "negate requires an integer or number";
  return false;
}

inline bool eval_size(const AutomationOpSpec &, const Args arguments,
                      AutomationValue &result, std::string &error) {
  if (const auto *v = std::get_if<std::string>(&arguments[0].storage)) {
    result = AutomationValue(static_cast<std::int64_t>(v->size()));
    return true;
  }
  if (const auto *v = std::get_if<AutomationList>(&arguments[0].storage)) {
    result = AutomationValue(static_cast<std::int64_t>(v->size()));
    return true;
  }
  if (const auto *v = std::get_if<AutomationMap>(&arguments[0].storage)) {
    result = AutomationValue(static_cast<std::int64_t>(v->size()));
    return true;
  }
  error = "size requires a collection";
  return false;
}

inline bool eval_and(const AutomationOpSpec &, const Args arguments,
                     AutomationValue &result, std::string &) {
  result = AutomationValue(truthy(arguments[0]) && truthy(arguments[1]));
  return true;
}

inline bool eval_or(const AutomationOpSpec &, const Args arguments,
                    AutomationValue &result, std::string &) {
  result = AutomationValue(truthy(arguments[0]) || truthy(arguments[1]));
  return true;
}

inline bool eval_equality(const AutomationOpSpec &self, const Args arguments,
                          AutomationValue &result, std::string &) {
  const bool equal = arguments[0] == arguments[1];
  result = AutomationValue(self.name == "equal" ? equal : !equal);
  return true;
}

inline bool eval_comparison(const AutomationOpSpec &self, const Args arguments,
                            AutomationValue &result, std::string &error) {
  const auto *left_integer = std::get_if<std::int64_t>(&arguments[0].storage);
  const auto *right_integer = std::get_if<std::int64_t>(&arguments[1].storage);
  if (left_integer != nullptr && right_integer != nullptr) {
    if (self.name == "less")
      result = AutomationValue(*left_integer < *right_integer);
    else if (self.name == "lessEqual")
      result = AutomationValue(*left_integer <= *right_integer);
    else if (self.name == "greater")
      result = AutomationValue(*left_integer > *right_integer);
    else if (self.name == "greaterEqual")
      result = AutomationValue(*left_integer >= *right_integer);
    else {
      error = "unsupported expression operation";
      return false;
    }
    return true;
  }
  const auto left = number(arguments[0]);
  const auto right = number(arguments[1]);
  if (!left || !right) {
    error = std::string(self.name) + " requires numeric arguments";
    return false;
  }
  if (self.name == "less")
    result = AutomationValue(*left < *right);
  else if (self.name == "lessEqual")
    result = AutomationValue(*left <= *right);
  else if (self.name == "greater")
    result = AutomationValue(*left > *right);
  else if (self.name == "greaterEqual")
    result = AutomationValue(*left >= *right);
  else {
    error = "unsupported expression operation";
    return false;
  }
  return true;
}

inline bool eval_arithmetic(const AutomationOpSpec &self, const Args arguments,
                            AutomationValue &result, std::string &error) {
  const auto *left_integer = std::get_if<std::int64_t>(&arguments[0].storage);
  const auto *right_integer = std::get_if<std::int64_t>(&arguments[1].storage);
  if (left_integer != nullptr && right_integer != nullptr &&
      self.name != "divide") {
    const auto value =
        checked_integer_binary(*left_integer, *right_integer, self.name);
    if (!value.has_value()) {
      error = "integer arithmetic overflow or invalid divisor";
      return false;
    }
    result = AutomationValue(*value);
    return true;
  }

  const auto left = number(arguments[0]);
  const auto right = number(arguments[1]);
  if (!left || !right) {
    error = std::string(self.name) + " requires numeric arguments";
    return false;
  }
  const bool duration_operands =
      std::holds_alternative<DurationValue>(arguments[0].storage) ||
      std::holds_alternative<DurationValue>(arguments[1].storage);
  if (duration_operands &&
      (!std::holds_alternative<DurationValue>(arguments[0].storage) ||
       !std::holds_alternative<DurationValue>(arguments[1].storage) ||
       (self.name != "add" && self.name != "subtract"))) {
    error = "durations only support add and subtract";
    return false;
  }

  double value{};
  if (self.name == "add")
    value = *left + *right;
  else if (self.name == "subtract")
    value = *left - *right;
  else if (self.name == "multiply")
    value = *left * *right;
  else if (self.name == "divide") {
    if (*right == 0.0) {
      error = "division by zero";
      return false;
    }
    value = *left / *right;
  } else if (self.name == "modulo") {
    if (*right == 0.0) {
      error = "modulo by zero";
      return false;
    }
    value = std::fmod(*left, *right);
  } else {
    error = "unsupported expression operation";
    return false;
  }
  if (duration_operands) {
    auto duration = duration_result(value);
    if (!duration.has_value()) {
      error = "duration arithmetic is out of range";
      return false;
    }
    result = *duration;
    return true;
  }
  if (!std::isfinite(value)) {
    error = "numeric result is not finite";
    return false;
  }
  result = AutomationValue(value);
  return true;
}

inline bool eval_any(const AutomationOpSpec &self, const Args arguments,
                     AutomationValue &result, std::string &error) {
  const auto *list = std::get_if<AutomationList>(&arguments[0].storage);
  if (list == nullptr) {
    error = std::string(self.name) + " requires a list";
    return false;
  }
  result = AutomationValue(std::ranges::any_of(*list, truthy));
  return true;
}

inline bool eval_all(const AutomationOpSpec &self, const Args arguments,
                     AutomationValue &result, std::string &error) {
  const auto *list = std::get_if<AutomationList>(&arguments[0].storage);
  if (list == nullptr) {
    error = std::string(self.name) + " requires a list";
    return false;
  }
  result = AutomationValue(std::ranges::all_of(*list, truthy));
  return true;
}

inline bool eval_map_filter(const AutomationOpSpec &self, const Args arguments,
                            AutomationValue &result, std::string &error) {
  const auto *list = std::get_if<AutomationList>(&arguments[0].storage);
  if (list == nullptr) {
    error = std::string(self.name) + " requires a list";
    return false;
  }
  const auto *field = std::get_if<std::string>(&arguments[1].storage);
  if (field == nullptr) {
    error = std::string(self.name) + " requires a string field name";
    return false;
  }
  AutomationList items;
  items.reserve(list->size());
  for (const auto &item : *list) {
    const auto *map = std::get_if<AutomationMap>(&item.storage);
    const auto found =
        map == nullptr ? AutomationMap::const_iterator{} : map->find(*field);
    if (self.name == "map") {
      items.push_back(map != nullptr && found != map->end() ? found->second
                                                            : AutomationValue{});
    } else if (map != nullptr && found != map->end() &&
               truthy(found->second)) {
      items.push_back(item);
    }
  }
  result = AutomationValue(std::move(items));
  return true;
}

inline bool eval_min_max_clamp(const AutomationOpSpec &self, const Args arguments,
                               AutomationValue &result, std::string &error) {
  auto value = number(arguments[0]);
  if (!value.has_value()) {
    error = std::string(self.name) + " requires numbers";
    return false;
  }
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const auto next = number(arguments[index]);
    if (!next.has_value()) {
      error = std::string(self.name) + " requires numbers";
      return false;
    }
    if (self.name == "min")
      *value = std::min(*value, *next);
    else if (self.name == "max")
      *value = std::max(*value, *next);
    else if (index == 1)
      *value = std::max(*value, *next);
    else
      *value = std::min(*value, *next);
  }
  result = AutomationValue(*value);
  return true;
}

} // namespace op_impl

inline constexpr std::array<AutomationOpSpec, 23> automation_ops{{
    {"not", ExpressionKind::unary, 1, 1, op_impl::infer_boolean_unary,
     op_impl::eval_not},
    {"negate", ExpressionKind::unary, 1, 1, op_impl::infer_negate,
     op_impl::eval_negate},
    {"size", ExpressionKind::unary, 1, 1, op_impl::infer_size,
     op_impl::eval_size},
    {"add", ExpressionKind::binary, 2, 2, op_impl::infer_arithmetic,
     op_impl::eval_arithmetic},
    {"subtract", ExpressionKind::binary, 2, 2, op_impl::infer_arithmetic,
     op_impl::eval_arithmetic},
    {"multiply", ExpressionKind::binary, 2, 2, op_impl::infer_arithmetic,
     op_impl::eval_arithmetic},
    {"divide", ExpressionKind::binary, 2, 2, op_impl::infer_arithmetic,
     op_impl::eval_arithmetic},
    {"modulo", ExpressionKind::binary, 2, 2, op_impl::infer_arithmetic,
     op_impl::eval_arithmetic},
    {"equal", ExpressionKind::binary, 2, 2, op_impl::infer_equality,
     op_impl::eval_equality},
    {"notEqual", ExpressionKind::binary, 2, 2, op_impl::infer_equality,
     op_impl::eval_equality},
    {"less", ExpressionKind::binary, 2, 2, op_impl::infer_comparison,
     op_impl::eval_comparison},
    {"lessEqual", ExpressionKind::binary, 2, 2, op_impl::infer_comparison,
     op_impl::eval_comparison},
    {"greater", ExpressionKind::binary, 2, 2, op_impl::infer_comparison,
     op_impl::eval_comparison},
    {"greaterEqual", ExpressionKind::binary, 2, 2, op_impl::infer_comparison,
     op_impl::eval_comparison},
    {"and", ExpressionKind::binary, 2, 2, op_impl::infer_boolean_binary,
     op_impl::eval_and},
    {"or", ExpressionKind::binary, 2, 2, op_impl::infer_boolean_binary,
     op_impl::eval_or},
    {"min", ExpressionKind::call, 2, 3, op_impl::infer_min_max_clamp,
     op_impl::eval_min_max_clamp},
    {"max", ExpressionKind::call, 2, 3, op_impl::infer_min_max_clamp,
     op_impl::eval_min_max_clamp},
    {"clamp", ExpressionKind::call, 3, 3, op_impl::infer_min_max_clamp,
     op_impl::eval_min_max_clamp},
    {"any", ExpressionKind::call, 1, 1, op_impl::infer_any_all,
     op_impl::eval_any},
    {"all", ExpressionKind::call, 1, 1, op_impl::infer_any_all,
     op_impl::eval_all},
    {"map", ExpressionKind::call, 2, 2, op_impl::infer_map_filter,
     op_impl::eval_map_filter},
    {"filter", ExpressionKind::call, 2, 2, op_impl::infer_map_filter,
     op_impl::eval_map_filter},
}};

[[nodiscard]] inline const AutomationOpSpec *
find_automation_op(const std::string_view name) noexcept {
  for (const auto &spec : automation_ops)
    if (spec.name == name)
      return &spec;
  return nullptr;
}

} // namespace layerc::core::detail
