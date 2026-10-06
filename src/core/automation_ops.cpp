#include "automation_ops.hpp"

#include <algorithm>
#include <array>
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

namespace {
namespace op_impl {
using Args = std::span<const AutomationValue>;
using Types = std::span<const std::optional<AutomationValueType>>;

bool known(const Types types, const std::size_t index,
                  const AutomationValueType expected) {
  return !types[index].has_value() || *types[index] == expected;
}

bool numeric(const Types types, const std::size_t index) {
  return !types[index].has_value() || numeric_type(*types[index]);
}

AutomationOpTypeResult infer_boolean_unary(const AutomationOpSpec &self, const Types types) {
  std::optional<AutomationValueType> result;
  if (!known(types, 0, AutomationValueType::boolean)) {
    return AutomationError{"$runtime", std::string(self.name) + " requires a Boolean"};
  }
  result = AutomationValueType::boolean;
  return result;
}

AutomationOpTypeResult infer_negate(const AutomationOpSpec &, const Types types) {
  std::optional<AutomationValueType> result;
  if (types[0].has_value() && *types[0] != AutomationValueType::integer &&
      *types[0] != AutomationValueType::number) {
    return AutomationError{"$runtime", "negate requires an integer or number"};
  }
  result = types[0];
  return result;
}

AutomationOpTypeResult infer_size(const AutomationOpSpec &, const Types types) {
  std::optional<AutomationValueType> result;
  if (types[0].has_value() && *types[0] != AutomationValueType::string &&
      *types[0] != AutomationValueType::list &&
      *types[0] != AutomationValueType::map) {
    return AutomationError{"$runtime", "size requires a string, list, or map"};
  }
  result = AutomationValueType::integer;
  return result;
}

AutomationOpTypeResult infer_boolean_binary(const AutomationOpSpec &self, const Types types) {
  std::optional<AutomationValueType> result;
  if (!known(types, 0, AutomationValueType::boolean) ||
      !known(types, 1, AutomationValueType::boolean)) {
    return AutomationError{"$runtime", std::string(self.name) + " requires Booleans"};
  }
  result = AutomationValueType::boolean;
  return result;
}

AutomationOpTypeResult infer_equality(const AutomationOpSpec &, const Types types) {
  std::optional<AutomationValueType> result;
  if (types[0].has_value() && types[1].has_value() &&
      *types[0] != *types[1]) {
    return AutomationError{"$runtime", "equality operands must have the same type"};
  }
  result = AutomationValueType::boolean;
  return result;
}

AutomationOpTypeResult infer_comparison(const AutomationOpSpec &self, const Types types) {
  std::optional<AutomationValueType> result;
  if (!numeric(types, 0) || !numeric(types, 1)) {
    return AutomationError{"$runtime", std::string(self.name) + " requires numeric operands"};
  }
  result = AutomationValueType::boolean;
  return result;
}

AutomationOpTypeResult infer_arithmetic(const AutomationOpSpec &self, const Types types) {
  std::optional<AutomationValueType> result;
  if (!numeric(types, 0) || !numeric(types, 1)) {
    return AutomationError{"$runtime", std::string(self.name) + " requires numeric operands"};
  }
  const bool left_duration = types[0] == AutomationValueType::duration;
  const bool right_duration = types[1] == AutomationValueType::duration;
  if ((left_duration || right_duration) &&
      (!left_duration || !right_duration ||
       (self.opcode != AutomationOpcode::add &&
        self.opcode != AutomationOpcode::subtract))) {
    return AutomationError{"$runtime", "durations only support add and subtract"};
  }
  if (self.opcode != AutomationOpcode::divide &&
      types[0] == AutomationValueType::integer &&
      types[1] == AutomationValueType::integer)
    result = AutomationValueType::integer;
  else if (left_duration && right_duration)
    result = AutomationValueType::duration;
  else
    result = AutomationValueType::number;
  return result;
}

AutomationOpTypeResult infer_any_all(const AutomationOpSpec &self, const Types types) {
  std::optional<AutomationValueType> result;
  if (types[0].has_value() && *types[0] != AutomationValueType::list) {
    return AutomationError{"$runtime", std::string(self.name) + " requires one list"};
  }
  result = AutomationValueType::boolean;
  return result;
}

AutomationOpTypeResult infer_map_filter(const AutomationOpSpec &self, const Types types) {
  std::optional<AutomationValueType> result;
  if ((types[0].has_value() && *types[0] != AutomationValueType::list) ||
      !known(types, 1, AutomationValueType::string)) {
    return AutomationError{"$runtime", std::string(self.name) + " requires a list and a string field name"};
  }
  result = AutomationValueType::list;
  return result;
}

AutomationOpTypeResult infer_min_max_clamp(const AutomationOpSpec &self, const Types types) {
  std::optional<AutomationValueType> result;
  if (!std::ranges::all_of(
          std::views::iota(std::size_t{}, types.size()),
          [&](const std::size_t index) { return numeric(types, index); })) {
    return AutomationError{"$runtime", std::string(self.name) + " requires bounded numeric arguments"};
  }
  result = AutomationValueType::number;
  return result;
}

// --- evaluation ---

AutomationOpResult eval_not(const AutomationOpSpec &, const Args arguments) {
  AutomationValue result;
  result = AutomationValue(!truthy(arguments[0]));
  return result;
}

AutomationOpResult eval_negate(const AutomationOpSpec &, const Args arguments) {
  AutomationValue result;
  if (const auto *integer = std::get_if<std::int64_t>(&arguments[0].storage)) {
    if (*integer == std::numeric_limits<std::int64_t>::min()) {
      return AutomationError{"$runtime", "integer negation overflow"};
    }
    result = AutomationValue(-*integer);
    return result;
  }
  if (const auto *value = std::get_if<double>(&arguments[0].storage)) {
    if (!std::isfinite(-*value)) {
      return AutomationError{"$runtime", "numeric result is not finite"};
    }
    result = AutomationValue(-*value);
    return result;
  }
  return AutomationError{"$runtime", "negate requires an integer or number"};
}

AutomationOpResult eval_size(const AutomationOpSpec &, const Args arguments) {
  AutomationValue result;
  if (const auto *v = std::get_if<std::string>(&arguments[0].storage)) {
    result = AutomationValue(static_cast<std::int64_t>(v->size()));
    return result;
  }
  if (const auto *v = std::get_if<AutomationList>(&arguments[0].storage)) {
    result = AutomationValue(static_cast<std::int64_t>(v->size()));
    return result;
  }
  if (const auto *v = std::get_if<AutomationMap>(&arguments[0].storage)) {
    result = AutomationValue(static_cast<std::int64_t>(v->size()));
    return result;
  }
  return AutomationError{"$runtime", "size requires a collection"};
}

AutomationOpResult eval_and(const AutomationOpSpec &, const Args arguments) {
  AutomationValue result;
  result = AutomationValue(truthy(arguments[0]) && truthy(arguments[1]));
  return result;
}

AutomationOpResult eval_or(const AutomationOpSpec &, const Args arguments) {
  AutomationValue result;
  result = AutomationValue(truthy(arguments[0]) || truthy(arguments[1]));
  return result;
}

AutomationOpResult eval_equality(const AutomationOpSpec &self, const Args arguments) {
  AutomationValue result;
  const bool equal = arguments[0] == arguments[1];
  result = AutomationValue(self.opcode == AutomationOpcode::equal ? equal
                                                                   : !equal);
  return result;
}

AutomationOpResult eval_comparison(const AutomationOpSpec &self, const Args arguments) {
  AutomationValue result;
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
  const auto finish = [&](const auto left, const auto right) -> AutomationOpResult {
    const auto compared = compare(left, right);
    if (!compared.has_value()) {
      return AutomationError{"$runtime", "unsupported expression operation"};
    }
    result = AutomationValue(*compared);
    return result;
  };
  const auto *left_integer = std::get_if<std::int64_t>(&arguments[0].storage);
  const auto *right_integer = std::get_if<std::int64_t>(&arguments[1].storage);
  if (left_integer != nullptr && right_integer != nullptr)
    return finish(*left_integer, *right_integer);
  const auto left = number(arguments[0]);
  const auto right = number(arguments[1]);
  if (!left || !right) {
    return AutomationError{"$runtime", std::string(self.name) + " requires numeric arguments"};
  }
  return finish(*left, *right);
}

AutomationOpResult eval_arithmetic(const AutomationOpSpec &self, const Args arguments) {
  AutomationValue result;
  const auto *left_integer = std::get_if<std::int64_t>(&arguments[0].storage);
  const auto *right_integer = std::get_if<std::int64_t>(&arguments[1].storage);
  if (left_integer != nullptr && right_integer != nullptr &&
      self.opcode != AutomationOpcode::divide) {
    const auto value =
        checked_integer_binary(*left_integer, *right_integer, self.opcode);
    if (!value.has_value()) {
      return AutomationError{"$runtime", "integer arithmetic overflow or invalid divisor"};
    }
    result = AutomationValue(*value);
    return result;
  }

  const auto left = number(arguments[0]);
  const auto right = number(arguments[1]);
  if (!left || !right) {
    return AutomationError{"$runtime", std::string(self.name) + " requires numeric arguments"};
  }
  const bool duration_operands =
      std::holds_alternative<DurationValue>(arguments[0].storage) ||
      std::holds_alternative<DurationValue>(arguments[1].storage);
  if (duration_operands &&
      (!std::holds_alternative<DurationValue>(arguments[0].storage) ||
       !std::holds_alternative<DurationValue>(arguments[1].storage) ||
       (self.opcode != AutomationOpcode::add &&
        self.opcode != AutomationOpcode::subtract))) {
    return AutomationError{"$runtime", "durations only support add and subtract"};
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
      return AutomationError{"$runtime", "division by zero"};
    }
    value = *left / *right;
    break;
  case AutomationOpcode::modulo:
    if (*right == 0.0) {
      return AutomationError{"$runtime", "modulo by zero"};
    }
    value = std::fmod(*left, *right);
    break;
  default:
    return AutomationError{"$runtime", "unsupported expression operation"};
  }
  if (duration_operands) {
    auto duration = duration_result(value);
    if (!duration.has_value()) {
      return AutomationError{"$runtime", "duration arithmetic is out of range"};
    }
    result = *duration;
    return result;
  }
  if (!std::isfinite(value)) {
    return AutomationError{"$runtime", "numeric result is not finite"};
  }
  result = AutomationValue(value);
  return result;
}

AutomationOpResult eval_any(const AutomationOpSpec &self, const Args arguments) {
  AutomationValue result;
  const auto *list = std::get_if<AutomationList>(&arguments[0].storage);
  if (list == nullptr)
    return AutomationError{"$runtime", std::string(self.name) + " requires a list"};
  result = AutomationValue(std::ranges::any_of(*list, truthy));
  return result;
}

AutomationOpResult eval_all(const AutomationOpSpec &self, const Args arguments) {
  AutomationValue result;
  const auto *list = std::get_if<AutomationList>(&arguments[0].storage);
  if (list == nullptr)
    return AutomationError{"$runtime", std::string(self.name) + " requires a list"};
  result = AutomationValue(std::ranges::all_of(*list, truthy));
  return result;
}

AutomationOpResult eval_map_filter(const AutomationOpSpec &self, const Args arguments) {
  AutomationValue result;
  const auto *list = std::get_if<AutomationList>(&arguments[0].storage);
  if (list == nullptr)
    return AutomationError{"$runtime", std::string(self.name) + " requires a list"};
  const auto *field = std::get_if<std::string>(&arguments[1].storage);
  if (field == nullptr) {
    return AutomationError{"$runtime", std::string(self.name) + " requires a string field name"};
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
  return result;
}

AutomationOpResult eval_min_max_clamp(const AutomationOpSpec &self, const Args arguments) {
  AutomationValue result;
  auto value = number(arguments[0]);
  if (!value.has_value()) {
    return AutomationError{"$runtime", std::string(self.name) + " requires numbers"};
  }
  // min/max fold every argument; clamp(x, lo, hi) is max with lo then min
  // with hi, so only the remaining bounds participate in order.
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const auto next = number(arguments[index]);
    if (!next.has_value()) {
      return AutomationError{"$runtime", std::string(self.name) + " requires numbers"};
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
  return result;
}

} // namespace op_impl

constexpr std::array<AutomationOpSpec, 23> automation_ops{{
    {"not", AutomationOpcode::logical_not, ExpressionKind::unary, 1, 1,
     op_impl::infer_boolean_unary, op_impl::eval_not},
    {"negate", AutomationOpcode::negate, ExpressionKind::unary, 1, 1,
     op_impl::infer_negate, op_impl::eval_negate},
    {"size", AutomationOpcode::size, ExpressionKind::unary, 1, 1,
     op_impl::infer_size, op_impl::eval_size},
    {"add", AutomationOpcode::add, ExpressionKind::binary, 2, 2,
     op_impl::infer_arithmetic, op_impl::eval_arithmetic},
    {"subtract", AutomationOpcode::subtract, ExpressionKind::binary, 2, 2,
     op_impl::infer_arithmetic, op_impl::eval_arithmetic},
    {"multiply", AutomationOpcode::multiply, ExpressionKind::binary, 2, 2,
     op_impl::infer_arithmetic, op_impl::eval_arithmetic},
    {"divide", AutomationOpcode::divide, ExpressionKind::binary, 2, 2,
     op_impl::infer_arithmetic, op_impl::eval_arithmetic},
    {"modulo", AutomationOpcode::modulo, ExpressionKind::binary, 2, 2,
     op_impl::infer_arithmetic, op_impl::eval_arithmetic},
    {"equal", AutomationOpcode::equal, ExpressionKind::binary, 2, 2,
     op_impl::infer_equality, op_impl::eval_equality},
    {"notEqual", AutomationOpcode::not_equal, ExpressionKind::binary, 2, 2,
     op_impl::infer_equality, op_impl::eval_equality},
    {"less", AutomationOpcode::less, ExpressionKind::binary, 2, 2,
     op_impl::infer_comparison, op_impl::eval_comparison},
    {"lessEqual", AutomationOpcode::less_equal, ExpressionKind::binary, 2, 2,
     op_impl::infer_comparison, op_impl::eval_comparison},
    {"greater", AutomationOpcode::greater, ExpressionKind::binary, 2, 2,
     op_impl::infer_comparison, op_impl::eval_comparison},
    {"greaterEqual", AutomationOpcode::greater_equal, ExpressionKind::binary, 2, 2,
     op_impl::infer_comparison, op_impl::eval_comparison},
    {"and", AutomationOpcode::logical_and, ExpressionKind::binary, 2, 2,
     op_impl::infer_boolean_binary, op_impl::eval_and},
    {"or", AutomationOpcode::logical_or, ExpressionKind::binary, 2, 2,
     op_impl::infer_boolean_binary, op_impl::eval_or},
    {"min", AutomationOpcode::minimum, ExpressionKind::call, 2, 3,
     op_impl::infer_min_max_clamp, op_impl::eval_min_max_clamp},
    {"max", AutomationOpcode::maximum, ExpressionKind::call, 2, 3,
     op_impl::infer_min_max_clamp, op_impl::eval_min_max_clamp},
    {"clamp", AutomationOpcode::clamp, ExpressionKind::call, 3, 3,
     op_impl::infer_min_max_clamp, op_impl::eval_min_max_clamp},
    {"any", AutomationOpcode::any, ExpressionKind::call, 1, 1,
     op_impl::infer_any_all, op_impl::eval_any},
    {"all", AutomationOpcode::all, ExpressionKind::call, 1, 1,
     op_impl::infer_any_all, op_impl::eval_all},
    {"map", AutomationOpcode::map, ExpressionKind::call, 2, 2,
     op_impl::infer_map_filter, op_impl::eval_map_filter},
    {"filter", AutomationOpcode::filter, ExpressionKind::call, 2, 2,
     op_impl::infer_map_filter, op_impl::eval_map_filter},
}};
} // namespace

const AutomationOpSpec *find_automation_op(const std::string_view name) noexcept {
  for (const auto &spec : automation_ops)
    if (spec.name == name) return &spec;
  return nullptr;
}

} // namespace layerc::core::detail
