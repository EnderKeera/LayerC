#pragma once

// One registry for expression operators: names, expression shape, static type
// inference, and evaluation all live here so the parser, the validator, and
// the runtime cannot drift apart. Structural expressions (literal, variable,
// event field, index) have distinct JSON shapes and stay hand-written at
// their call sites. Operator bodies live in automation_ops.cpp.

#include "layerc/core/automation.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace layerc::core::detail {

[[nodiscard]] bool truthy(const AutomationValue &value);
[[nodiscard]] std::optional<double> number(const AutomationValue &value);
[[nodiscard]] std::optional<AutomationValue> duration_result(double value);

enum class AutomationOpcode : std::uint8_t {
  logical_not,
  negate,
  size,
  add,
  subtract,
  multiply,
  divide,
  modulo,
  equal,
  not_equal,
  less,
  less_equal,
  greater,
  greater_equal,
  logical_and,
  logical_or,
  minimum,
  maximum,
  clamp,
  any,
  all,
  map,
  filter,
};

[[nodiscard]] std::optional<std::int64_t>
checked_integer_binary(std::int64_t left, std::int64_t right,
                       AutomationOpcode operation);

[[nodiscard]] bool numeric_type(AutomationValueType type);

struct AutomationOpSpec {
  std::string_view name;
  AutomationOpcode opcode;
  ExpressionKind kind;
  std::uint8_t min_args;
  std::uint8_t max_args;
  bool (*infer)(
      const AutomationOpSpec &self,
      std::span<const std::optional<AutomationValueType>> argument_types,
      std::optional<AutomationValueType> &result, std::string &error);
  bool (*evaluate)(const AutomationOpSpec &self,
                   std::span<const AutomationValue> arguments,
                   AutomationValue &result, std::string &error);
};

namespace op_impl {

using Args = std::span<const AutomationValue>;
using Types = std::span<const std::optional<AutomationValueType>>;

bool infer_boolean_unary(const AutomationOpSpec &, Types,
                         std::optional<AutomationValueType> &, std::string &);
bool infer_negate(const AutomationOpSpec &, Types,
                  std::optional<AutomationValueType> &, std::string &);
bool infer_size(const AutomationOpSpec &, Types,
                std::optional<AutomationValueType> &, std::string &);
bool infer_boolean_binary(const AutomationOpSpec &, Types,
                          std::optional<AutomationValueType> &, std::string &);
bool infer_equality(const AutomationOpSpec &, Types,
                    std::optional<AutomationValueType> &, std::string &);
bool infer_comparison(const AutomationOpSpec &, Types,
                      std::optional<AutomationValueType> &, std::string &);
bool infer_arithmetic(const AutomationOpSpec &, Types,
                      std::optional<AutomationValueType> &, std::string &);
bool infer_any_all(const AutomationOpSpec &, Types,
                   std::optional<AutomationValueType> &, std::string &);
bool infer_map_filter(const AutomationOpSpec &, Types,
                      std::optional<AutomationValueType> &, std::string &);
bool infer_min_max_clamp(const AutomationOpSpec &, Types,
                         std::optional<AutomationValueType> &, std::string &);

bool eval_not(const AutomationOpSpec &, Args, AutomationValue &, std::string &);
bool eval_negate(const AutomationOpSpec &, Args, AutomationValue &,
                 std::string &);
bool eval_size(const AutomationOpSpec &, Args, AutomationValue &, std::string &);
bool eval_and(const AutomationOpSpec &, Args, AutomationValue &, std::string &);
bool eval_or(const AutomationOpSpec &, Args, AutomationValue &, std::string &);
bool eval_equality(const AutomationOpSpec &, Args, AutomationValue &,
                   std::string &);
bool eval_comparison(const AutomationOpSpec &, Args, AutomationValue &,
                     std::string &);
bool eval_arithmetic(const AutomationOpSpec &, Args, AutomationValue &,
                     std::string &);
bool eval_any(const AutomationOpSpec &, Args, AutomationValue &, std::string &);
bool eval_all(const AutomationOpSpec &, Args, AutomationValue &, std::string &);
bool eval_map_filter(const AutomationOpSpec &, Args, AutomationValue &,
                     std::string &);
bool eval_min_max_clamp(const AutomationOpSpec &, Args, AutomationValue &,
                        std::string &);

} // namespace op_impl

inline constexpr std::array<AutomationOpSpec, 23> automation_ops{{
    {"not", AutomationOpcode::logical_not, ExpressionKind::unary, 1, 1,
     op_impl::infer_boolean_unary,
     op_impl::eval_not},
    {"negate", AutomationOpcode::negate, ExpressionKind::unary, 1, 1,
     op_impl::infer_negate,
     op_impl::eval_negate},
    {"size", AutomationOpcode::size, ExpressionKind::unary, 1, 1,
     op_impl::infer_size,
     op_impl::eval_size},
    {"add", AutomationOpcode::add, ExpressionKind::binary, 2, 2,
     op_impl::infer_arithmetic,
     op_impl::eval_arithmetic},
    {"subtract", AutomationOpcode::subtract, ExpressionKind::binary, 2, 2,
     op_impl::infer_arithmetic,
     op_impl::eval_arithmetic},
    {"multiply", AutomationOpcode::multiply, ExpressionKind::binary, 2, 2,
     op_impl::infer_arithmetic,
     op_impl::eval_arithmetic},
    {"divide", AutomationOpcode::divide, ExpressionKind::binary, 2, 2,
     op_impl::infer_arithmetic,
     op_impl::eval_arithmetic},
    {"modulo", AutomationOpcode::modulo, ExpressionKind::binary, 2, 2,
     op_impl::infer_arithmetic,
     op_impl::eval_arithmetic},
    {"equal", AutomationOpcode::equal, ExpressionKind::binary, 2, 2,
     op_impl::infer_equality,
     op_impl::eval_equality},
    {"notEqual", AutomationOpcode::not_equal, ExpressionKind::binary, 2, 2,
     op_impl::infer_equality,
     op_impl::eval_equality},
    {"less", AutomationOpcode::less, ExpressionKind::binary, 2, 2,
     op_impl::infer_comparison,
     op_impl::eval_comparison},
    {"lessEqual", AutomationOpcode::less_equal, ExpressionKind::binary, 2, 2,
     op_impl::infer_comparison,
     op_impl::eval_comparison},
    {"greater", AutomationOpcode::greater, ExpressionKind::binary, 2, 2,
     op_impl::infer_comparison,
     op_impl::eval_comparison},
    {"greaterEqual", AutomationOpcode::greater_equal, ExpressionKind::binary,
     2, 2, op_impl::infer_comparison,
     op_impl::eval_comparison},
    {"and", AutomationOpcode::logical_and, ExpressionKind::binary, 2, 2,
     op_impl::infer_boolean_binary,
     op_impl::eval_and},
    {"or", AutomationOpcode::logical_or, ExpressionKind::binary, 2, 2,
     op_impl::infer_boolean_binary,
     op_impl::eval_or},
    {"min", AutomationOpcode::minimum, ExpressionKind::call, 2, 3,
     op_impl::infer_min_max_clamp,
     op_impl::eval_min_max_clamp},
    {"max", AutomationOpcode::maximum, ExpressionKind::call, 2, 3,
     op_impl::infer_min_max_clamp,
     op_impl::eval_min_max_clamp},
    {"clamp", AutomationOpcode::clamp, ExpressionKind::call, 3, 3,
     op_impl::infer_min_max_clamp,
     op_impl::eval_min_max_clamp},
    {"any", AutomationOpcode::any, ExpressionKind::call, 1, 1,
     op_impl::infer_any_all,
     op_impl::eval_any},
    {"all", AutomationOpcode::all, ExpressionKind::call, 1, 1,
     op_impl::infer_any_all,
     op_impl::eval_all},
    {"map", AutomationOpcode::map, ExpressionKind::call, 2, 2,
     op_impl::infer_map_filter,
     op_impl::eval_map_filter},
    {"filter", AutomationOpcode::filter, ExpressionKind::call, 2, 2,
     op_impl::infer_map_filter,
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
