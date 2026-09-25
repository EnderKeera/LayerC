#include "automation_ops.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

namespace layerc::core::detail {

[[nodiscard]] bool truthy(const AutomationValue &value) {
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

[[nodiscard]] std::optional<double> number(const AutomationValue &value) {
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

[[nodiscard]] std::optional<AutomationValue>
duration_result(const double value) {
  if (!std::isfinite(value) || value < 0.0 ||
      value > static_cast<double>(max_cooldown_duration.count()) ||
      std::floor(value) != value)
    return std::nullopt;
  return AutomationValue(
      DurationValue{Milliseconds(static_cast<std::int64_t>(value))});
}

[[nodiscard]] std::optional<std::int64_t>
checked_integer_binary(const std::int64_t left, const std::int64_t right,
                       const AutomationOpcode operation) {
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  switch (operation) {
  case AutomationOpcode::add:
    if ((right > 0 && left > maximum - right) ||
        (right < 0 && left < minimum - right))
      return std::nullopt;
    return left + right;
  case AutomationOpcode::subtract:
    if ((right < 0 && left > maximum + right) ||
        (right > 0 && left < minimum + right))
      return std::nullopt;
    return left - right;
  case AutomationOpcode::multiply:
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
  case AutomationOpcode::modulo:
    if (right == 0 || (left == minimum && right == -1))
      return std::nullopt;
    return left % right;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] bool
numeric_type(const AutomationValueType type) {
  return type == AutomationValueType::integer ||
         type == AutomationValueType::number ||
         type == AutomationValueType::duration ||
         type == AutomationValueType::confidence;
}

namespace op_impl {

bool known(const Types types, const std::size_t index,
                  const AutomationValueType expected) {
  return !types[index].has_value() || *types[index] == expected;
}

bool numeric(const Types types, const std::size_t index) {
  return !types[index].has_value() || numeric_type(*types[index]);
}

bool infer_boolean_unary(const AutomationOpSpec &self, const Types types,
                                std::optional<AutomationValueType> &result,
                                std::string &error) {
  if (!known(types, 0, AutomationValueType::boolean)) {
    error = std::string(self.name) + " requires a Boolean";
    return false;
  }
  result = AutomationValueType::boolean;
  return true;
}

bool infer_negate(const AutomationOpSpec &, const Types types,
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

bool infer_size(const AutomationOpSpec &, const Types types,
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

bool infer_boolean_binary(const AutomationOpSpec &self, const Types types,
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

bool infer_equality(const AutomationOpSpec &, const Types types,
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

bool infer_comparison(const AutomationOpSpec &self, const Types types,
                             std::optional<AutomationValueType> &result,
                             std::string &error) {
  if (!numeric(types, 0) || !numeric(types, 1)) {
    error = std::string(self.name) + " requires numeric operands";
    return false;
  }
  result = AutomationValueType::boolean;
  return true;
}

bool infer_arithmetic(const AutomationOpSpec &self, const Types types,
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
       (self.opcode != AutomationOpcode::add &&
        self.opcode != AutomationOpcode::subtract))) {
    error = "durations only support add and subtract";
    return false;
  }
  if (self.opcode != AutomationOpcode::divide &&
      types[0] == AutomationValueType::integer &&
      types[1] == AutomationValueType::integer)
    result = AutomationValueType::integer;
  else if (left_duration && right_duration)
    result = AutomationValueType::duration;
  else
    result = AutomationValueType::number;
  return true;
}

bool infer_any_all(const AutomationOpSpec &self, const Types types,
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

bool infer_map_filter(const AutomationOpSpec &self, const Types types,
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

bool infer_min_max_clamp(const AutomationOpSpec &self, const Types types,
                                std::optional<AutomationValueType> &result,
                                std::string &error) {
  const bool valid_count =
      (self.opcode == AutomationOpcode::clamp && types.size() == 3U) ||
      ((self.opcode == AutomationOpcode::minimum ||
        self.opcode == AutomationOpcode::maximum) &&
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

bool eval_not(const AutomationOpSpec &, const Args arguments,
                     AutomationValue &result, std::string &) {
  result = AutomationValue(!truthy(arguments[0]));
  return true;
}

bool eval_negate(const AutomationOpSpec &, const Args arguments,
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

bool eval_size(const AutomationOpSpec &, const Args arguments,
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

bool eval_and(const AutomationOpSpec &, const Args arguments,
                     AutomationValue &result, std::string &) {
  result = AutomationValue(truthy(arguments[0]) && truthy(arguments[1]));
  return true;
}

bool eval_or(const AutomationOpSpec &, const Args arguments,
                    AutomationValue &result, std::string &) {
  result = AutomationValue(truthy(arguments[0]) || truthy(arguments[1]));
  return true;
}

bool eval_equality(const AutomationOpSpec &self, const Args arguments,
                           AutomationValue &result, std::string &) {
  const bool equal = arguments[0] == arguments[1];
  result = AutomationValue(self.opcode == AutomationOpcode::equal ? equal
                                                                   : !equal);
  return true;
}

bool eval_comparison(const AutomationOpSpec &self, const Args arguments,
                             AutomationValue &result, std::string &error) {
  const auto compare = [operation = self.opcode](const auto left,
                                                  const auto right)
      -> std::optional<bool> {
    switch (operation) {
    case AutomationOpcode::less: return left < right;
    case AutomationOpcode::less_equal: return left <= right;
    case AutomationOpcode::greater: return left > right;
    case AutomationOpcode::greater_equal: return left >= right;
    default: return std::nullopt;
    }
  };
  const auto finish = [&](const auto left, const auto right) {
    const auto compared = compare(left, right);
    if (!compared.has_value()) {
      error = "unsupported expression operation";
      return false;
    }
    result = AutomationValue(*compared);
    return true;
  };
  const auto *left_integer = std::get_if<std::int64_t>(&arguments[0].storage);
  const auto *right_integer = std::get_if<std::int64_t>(&arguments[1].storage);
  if (left_integer != nullptr && right_integer != nullptr)
    return finish(*left_integer, *right_integer);
  const auto left = number(arguments[0]);
  const auto right = number(arguments[1]);
  if (!left || !right) {
    error = std::string(self.name) + " requires numeric arguments";
    return false;
  }
  return finish(*left, *right);
}

bool eval_arithmetic(const AutomationOpSpec &self, const Args arguments,
                            AutomationValue &result, std::string &error) {
  const auto *left_integer = std::get_if<std::int64_t>(&arguments[0].storage);
  const auto *right_integer = std::get_if<std::int64_t>(&arguments[1].storage);
  if (left_integer != nullptr && right_integer != nullptr &&
      self.opcode != AutomationOpcode::divide) {
    const auto value =
        checked_integer_binary(*left_integer, *right_integer, self.opcode);
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
       (self.opcode != AutomationOpcode::add &&
        self.opcode != AutomationOpcode::subtract))) {
    error = "durations only support add and subtract";
    return false;
  }

  double value{};
  switch (self.opcode) {
  case AutomationOpcode::add:
    value = *left + *right;
    break;
  case AutomationOpcode::subtract:
    value = *left - *right;
    break;
  case AutomationOpcode::multiply:
    value = *left * *right;
    break;
  case AutomationOpcode::divide:
    if (*right == 0.0) {
      error = "division by zero";
      return false;
    }
    value = *left / *right;
    break;
  case AutomationOpcode::modulo:
    if (*right == 0.0) {
      error = "modulo by zero";
      return false;
    }
    value = std::fmod(*left, *right);
    break;
  default:
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

namespace {

const AutomationList *list_argument(const AutomationOpSpec &self,
                                   const AutomationValue &argument,
                                   std::string &error) {
  const auto *list = std::get_if<AutomationList>(&argument.storage);
  if (list == nullptr)
    error = std::string(self.name) + " requires a list";
  return list;
}

} // namespace

bool eval_any(const AutomationOpSpec &self, const Args arguments,
                     AutomationValue &result, std::string &error) {
  const auto *list = list_argument(self, arguments[0], error);
  if (list == nullptr)
    return false;
  result = AutomationValue(std::ranges::any_of(*list, truthy));
  return true;
}

bool eval_all(const AutomationOpSpec &self, const Args arguments,
                     AutomationValue &result, std::string &error) {
  const auto *list = list_argument(self, arguments[0], error);
  if (list == nullptr)
    return false;
  result = AutomationValue(std::ranges::all_of(*list, truthy));
  return true;
}

bool eval_map_filter(const AutomationOpSpec &self, const Args arguments,
                            AutomationValue &result, std::string &error) {
  const auto *list = list_argument(self, arguments[0], error);
  if (list == nullptr)
    return false;
  const auto *field = std::get_if<std::string>(&arguments[1].storage);
  if (field == nullptr) {
    error = std::string(self.name) + " requires a string field name";
    return false;
  }
  AutomationList items;
  items.reserve(list->size());
  for (const auto &item : *list) {
    const auto *map = std::get_if<AutomationMap>(&item.storage);
    const AutomationValue *field_value = nullptr;
    if (map != nullptr) {
      if (const auto found = map->find(*field); found != map->end())
        field_value = &found->second;
    }
    if (self.opcode == AutomationOpcode::map)
      items.push_back(field_value != nullptr ? *field_value : AutomationValue{});
    else if (field_value != nullptr && truthy(*field_value))
      items.push_back(item);
  }
  result = AutomationValue(std::move(items));
  return true;
}

bool eval_min_max_clamp(const AutomationOpSpec &self, const Args arguments,
                               AutomationValue &result, std::string &error) {
  auto value = number(arguments[0]);
  if (!value.has_value()) {
    error = std::string(self.name) + " requires numbers";
    return false;
  }
  // min/max fold every argument; clamp(x, lo, hi) is max with lo then min
  // with hi, so only the remaining bounds participate in order.
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const auto next = number(arguments[index]);
    if (!next.has_value()) {
      error = std::string(self.name) + " requires numbers";
      return false;
    }
    if (self.opcode == AutomationOpcode::clamp) {
      if (index == 1)
        *value = std::max(*value, *next);
      else
        *value = std::min(*value, *next);
    } else if (self.opcode == AutomationOpcode::minimum) {
      *value = std::min(*value, *next);
    } else {
      *value = std::max(*value, *next);
    }
  }
  result = AutomationValue(*value);
  return true;
}

} // namespace op_impl

} // namespace layerc::core::detail
