#pragma once

#include "layerc/core/cooldown_groups.hpp"
#include "layerc/core/document_error.hpp"
#include "layerc/core/panel_presentation.hpp"
#include "layerc/core/types.hpp"
#include "layerc/core/enum_wire.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace layerc::core {
namespace json { class Value; }

inline constexpr std::size_t max_automation_json_bytes = 256U * 1024U;
// A runtime keeps at most this many trace entries and drops the oldest
// trim-sized block when full; imported traces have the same bound.
inline constexpr std::size_t max_automation_trace_entries = 2048;
inline constexpr std::size_t automation_trace_trim_entries = 256;
inline constexpr std::size_t max_automation_state_bytes = 64U * 1024U;
inline constexpr std::size_t max_automation_trace_bytes = 4U * 1024U * 1024U;
inline constexpr std::size_t max_automation_rules = 64;
inline constexpr std::size_t max_automation_variables = 64;
inline constexpr std::size_t max_automation_nodes_per_rule = 256;
inline constexpr std::size_t max_automation_ast_depth = 16;
inline constexpr std::size_t max_automation_collection_entries = 64;
inline constexpr std::size_t max_automation_string_bytes = 512;
inline constexpr std::size_t max_automation_actions_per_rule = 32;
inline constexpr std::size_t max_automation_timers = 32;
inline constexpr std::size_t max_automation_node_evaluations = 8192;
inline constexpr std::size_t max_automation_derived_events = 32;
inline constexpr Milliseconds min_automation_timer_interval{50};
inline constexpr Milliseconds automation_execution_budget{2};

struct AutomationValue;
using AutomationList = std::vector<AutomationValue>;
using AutomationMap = std::map<std::string, AutomationValue, std::less<>>;

struct DurationValue {
  Milliseconds value{};
  bool operator==(const DurationValue &) const = default;
};
struct ConfidenceValue {
  double value{};
  bool operator==(const ConfidenceValue &) const = default;
};

struct AutomationValue {
  using Storage =
      std::variant<std::nullptr_t, bool, std::int64_t, double, DurationValue,
                   ConfidenceValue, std::string, AutomationList, AutomationMap>;
  Storage storage{};

  AutomationValue() = default;
  template <typename T>
  explicit AutomationValue(T value) : storage(std::move(value)) {}
  bool operator==(const AutomationValue &) const = default;
};

enum class AutomationValueType : std::uint8_t {
  null_value,
  boolean,
  integer,
  number,
  duration,
  confidence,
  string,
  list,
  map,
};

[[nodiscard]] AutomationValueType value_type(const AutomationValue &) noexcept;
[[nodiscard]] bool automation_value_valid(const AutomationValue &) noexcept;

[[nodiscard]] constexpr bool
automation_value_type_compatible(const AutomationValueType actual,
                                 const AutomationValueType expected) noexcept {
  return actual == expected || (actual == AutomationValueType::integer &&
                                expected == AutomationValueType::number);
}
// The sole declared promotion, applied once to evaluated action values.
[[nodiscard]] bool
normalize_automation_value(AutomationValue &value,
                           AutomationValueType expected) noexcept;

[[nodiscard]] std::string automation_value_json(const AutomationValue &);
// Interprets a duration-typed or non-negative integer value as a duration
// only when it is representable by the native monotonic clock.
[[nodiscard]] std::optional<Milliseconds>
as_duration(const AutomationValue &value) noexcept;
// Checked non-negative milliseconds conversion and addition. Also used with
// an epoch base to import trace timestamps without overflowing duration_cast.
[[nodiscard]] std::optional<TimePoint>
automation_time_after(TimePoint base, Milliseconds duration) noexcept;

enum class AutomationEventKind : std::uint8_t {
  input_pressed,
  input_released,
  input_value_changed,
  visual_observation,
  cooldown_triggered,
  cooldown_ready,
  group_triggered,
  group_ready,
  session_started,
  session_stopping,
  variable_changed,
  timer_fired,
};

struct AutomationEvent {
  AutomationEventKind kind{AutomationEventKind::session_started};
  TimePoint timestamp{};
  std::string source_id;
  std::uint64_t origin_token{};
  AutomationMap fields;
};

// The event-field vocabulary: producers emit these names, the validator types
// event-field expressions from them, and the simulator fills them in.
namespace automation_event_field {
inline constexpr std::string_view source_id = "sourceId";
inline constexpr std::string_view age_ms = "ageMs";
inline constexpr std::string_view previous = "previous";
inline constexpr std::string_view value = "value";
inline constexpr std::string_view old_value = "old";
inline constexpr std::string_view available = "available";
inline constexpr std::string_view matched = "matched";
inline constexpr std::string_view confidence = "confidence";
inline constexpr std::string_view late_by = "lateBy";
} // namespace automation_event_field

struct AutomationEventFieldSpec {
  std::string_view name;
  // nullopt: the field carries the changed variable's own type.
  std::optional<AutomationValueType> type;
};

// Fields every event exposes: sourceId is the event's source, and ageMs is
// recorded by replay (zero for live events).
inline constexpr std::array automation_common_event_fields{
    AutomationEventFieldSpec{automation_event_field::source_id,
                             AutomationValueType::string},
    AutomationEventFieldSpec{automation_event_field::age_ms,
                             AutomationValueType::duration}};

namespace detail {
inline constexpr std::array input_value_fields{
    AutomationEventFieldSpec{automation_event_field::previous,
                             AutomationValueType::number},
    AutomationEventFieldSpec{automation_event_field::value,
                             AutomationValueType::number}};
inline constexpr std::array visual_observation_fields{
    AutomationEventFieldSpec{automation_event_field::available,
                             AutomationValueType::boolean},
    AutomationEventFieldSpec{automation_event_field::matched,
                             AutomationValueType::boolean},
    AutomationEventFieldSpec{automation_event_field::confidence,
                             AutomationValueType::confidence}};
inline constexpr std::array variable_changed_fields{
    AutomationEventFieldSpec{automation_event_field::old_value, std::nullopt},
    AutomationEventFieldSpec{automation_event_field::value, std::nullopt}};
inline constexpr std::array timer_fired_fields{AutomationEventFieldSpec{
    automation_event_field::late_by, AutomationValueType::duration}};
} // namespace detail

// The fields one event kind carries beyond the common ones.
[[nodiscard]] constexpr std::span<const AutomationEventFieldSpec>
automation_event_fields(const AutomationEventKind kind) noexcept {
  switch (kind) {
  case AutomationEventKind::input_value_changed:
    return detail::input_value_fields;
  case AutomationEventKind::visual_observation:
    return detail::visual_observation_fields;
  case AutomationEventKind::variable_changed:
    return detail::variable_changed_fields;
  case AutomationEventKind::timer_fired:
    return detail::timer_fired_fields;
  default:
    return {};
  }
}

[[nodiscard]] constexpr const AutomationEventFieldSpec *
find_automation_event_field(const AutomationEventKind kind,
                            const std::string_view name) noexcept {
  for (const auto &field : automation_common_event_fields)
    if (field.name == name) return &field;
  for (const auto &field : automation_event_fields(kind))
    if (field.name == name) return &field;
  return nullptr;
}

// Wire-name vocabulary shared by the profile serializer, trace, packs and UIs.
inline constexpr std::array automation_event_wire{
    EnumWireEntry{AutomationEventKind::input_pressed, "inputPressed"},
    EnumWireEntry{AutomationEventKind::input_released, "inputReleased"},
    EnumWireEntry{AutomationEventKind::input_value_changed, "inputValueChanged"},
    EnumWireEntry{AutomationEventKind::visual_observation, "visualObservation"},
    EnumWireEntry{AutomationEventKind::cooldown_triggered, "cooldownTriggered"},
    EnumWireEntry{AutomationEventKind::cooldown_ready, "cooldownReady"},
    EnumWireEntry{AutomationEventKind::group_triggered, "groupTriggered"},
    EnumWireEntry{AutomationEventKind::group_ready, "groupReady"},
    EnumWireEntry{AutomationEventKind::session_started, "sessionStarted"},
    EnumWireEntry{AutomationEventKind::session_stopping, "sessionStopping"},
    EnumWireEntry{AutomationEventKind::variable_changed, "variableChanged"},
    EnumWireEntry{AutomationEventKind::timer_fired, "timerFired"}};
inline constexpr std::array automation_value_type_wire{
    EnumWireEntry{AutomationValueType::null_value, "null"},
    EnumWireEntry{AutomationValueType::boolean, "bool"},
    EnumWireEntry{AutomationValueType::integer, "int"},
    EnumWireEntry{AutomationValueType::number, "number"},
    EnumWireEntry{AutomationValueType::duration, "duration"},
    EnumWireEntry{AutomationValueType::confidence, "confidence"},
    EnumWireEntry{AutomationValueType::string, "string"},
    EnumWireEntry{AutomationValueType::list, "list"},
    EnumWireEntry{AutomationValueType::map, "map"}};

[[nodiscard]] inline std::string_view
automation_event_name(const AutomationEventKind kind) {
  return enum_wire_name(automation_event_wire, kind).value_or("sessionStarted");
}

// Canonical presentation order; kept as a derived array for editor indices.
inline constexpr auto automation_event_kinds = [] {
  std::array<AutomationEventKind, automation_event_wire.size()> kinds{};
  for (std::size_t i = 0; i < kinds.size(); ++i)
    kinds[i] = automation_event_wire[i].value;
  return kinds;
}();

[[nodiscard]] inline std::optional<AutomationEventKind>
parse_automation_event_name(const std::string_view name) {
  return parse_enum_wire(automation_event_wire, name);
}

[[nodiscard]] inline std::string_view
automation_value_type_name(const AutomationValueType type) {
  return enum_wire_name(automation_value_type_wire, type).value_or("null");
}

[[nodiscard]] inline std::optional<AutomationValueType>
parse_automation_value_type_name(const std::string_view name) {
  return parse_enum_wire(automation_value_type_wire, name);
}


enum class ExpressionKind : std::uint8_t {
  literal,
  variable,
  event_field,
  unary,
  binary,
  call,
  index,
};

struct AutomationExpression {
  ExpressionKind kind{ExpressionKind::literal};
  std::string operation{};
  std::string reference{};
  AutomationValue literal{};
  std::vector<AutomationExpression> arguments{};

  bool operator==(const AutomationExpression &) const = default;
};

// Builders for code-authored expressions (editor presets, fixtures, tools).
namespace expr {
[[nodiscard]] inline AutomationExpression literal(AutomationValue value) {
  return {.kind = ExpressionKind::literal, .literal = std::move(value)};
}
[[nodiscard]] inline AutomationExpression variable(std::string id) {
  return {.kind = ExpressionKind::variable, .reference = std::move(id)};
}
[[nodiscard]] inline AutomationExpression
binary(std::string operation, AutomationExpression left,
       AutomationExpression right) {
  return {.kind = ExpressionKind::binary,
          .operation = std::move(operation),
          .arguments = {std::move(left), std::move(right)}};
}
} // namespace expr

// Where an action's side effects land. Internal actions mutate only runtime
// state; cooldown actions commit through the engine adapter; overlay actions
// publish presentation overrides.
enum class AutomationEffectDomain : std::uint8_t {
  internal,
  cooldown_engine,
  overlay,
};

enum class AutomationActionKind : std::uint8_t {
  set_variable,
  increment_variable,
  append_list,
  set_map_entry,
  erase_map_entry,
  trigger_cooldown,
  reset_cooldown,
  reduce_cooldown_next,
  reduce_cooldown_all,
  refund_cooldown,
  trigger_group,
  reset_group,
  create_timer,
  cancel_timer,
  set_overlay_override,
  clear_overlay_override,
};

inline constexpr std::size_t automation_action_kind_count =
    static_cast<std::size_t>(AutomationActionKind::clear_overlay_override) + 1;

// How an action uses its secondary expression. One table drives parse,
// validate, serialize, and runtime so secondary rules cannot drift.
enum class ActionSecondaryPolicy : std::uint8_t {
  none,
  required_string_key,
  optional_duration,
};

// What an action's target string must name in the base profile (or, for
// timers/overrides, a free-form automation id).
enum class ActionTargetRole : std::uint8_t {
  variable_any,
  variable_numeric,
  variable_list,
  variable_map,
  cooldown,
  cooldown_group,
  timer_id,
  panel,
  override_name,
};

// Constraint on the evaluated primary value; none accepts no value.
enum class ActionValuePolicy : std::uint8_t {
  none,
  match_variable,
  match_numeric_variable,
  any,
  string_key,
  duration_or_integer,
  overlay_property,
};

struct AutomationActionSpec {
  AutomationActionKind kind;
  std::string_view wire_name;
  std::string_view display_name;
  AutomationEffectDomain domain;
  ActionSecondaryPolicy secondary;
  ActionTargetRole target;
  ActionValuePolicy value;
  bool allows_repeating{};
  bool requires_name{};
  bool requires_property{};
  // Accepts an optional itemId narrowing a panel target to one item.
  bool accepts_item{};

  // True for actions whose primary value expression is evaluated before the
  // transaction commits.
  [[nodiscard]] constexpr bool accepts_value() const noexcept {
    return value != ActionValuePolicy::none;
  }
};

inline constexpr std::array automation_action_specs{
    AutomationActionSpec{AutomationActionKind::set_variable, "setVariable",
                         "Set variable",
                         AutomationEffectDomain::internal,
                         ActionSecondaryPolicy::none,
                         ActionTargetRole::variable_any,
                         ActionValuePolicy::match_variable},
    AutomationActionSpec{AutomationActionKind::increment_variable,
                         "incrementVariable", "Increment variable",
                         AutomationEffectDomain::internal,
                         ActionSecondaryPolicy::none,
                         ActionTargetRole::variable_numeric,
                         ActionValuePolicy::match_numeric_variable},
    AutomationActionSpec{AutomationActionKind::append_list, "appendList",
                         "Append list",
                         AutomationEffectDomain::internal,
                         ActionSecondaryPolicy::none,
                         ActionTargetRole::variable_list,
                         ActionValuePolicy::any},
    AutomationActionSpec{AutomationActionKind::set_map_entry, "setMapEntry",
                         "Set map entry",
                         AutomationEffectDomain::internal,
                         ActionSecondaryPolicy::required_string_key,
                         ActionTargetRole::variable_map,
                         ActionValuePolicy::any},
    AutomationActionSpec{AutomationActionKind::erase_map_entry, "eraseMapEntry",
                         "Erase map entry",
                         AutomationEffectDomain::internal,
                         ActionSecondaryPolicy::none,
                         ActionTargetRole::variable_map,
                         ActionValuePolicy::string_key},
    AutomationActionSpec{AutomationActionKind::trigger_cooldown,
                         "triggerCooldown", "Trigger cooldown",
                         AutomationEffectDomain::cooldown_engine,
                         ActionSecondaryPolicy::none,
                         ActionTargetRole::cooldown, ActionValuePolicy::none},
    AutomationActionSpec{AutomationActionKind::reset_cooldown, "resetCooldown",
                         "Reset cooldown",
                         AutomationEffectDomain::cooldown_engine,
                         ActionSecondaryPolicy::none,
                         ActionTargetRole::cooldown, ActionValuePolicy::none},
    AutomationActionSpec{AutomationActionKind::reduce_cooldown_next,
                         "reduceCooldownNext", "Reduce next recovery",
                         AutomationEffectDomain::cooldown_engine,
                         ActionSecondaryPolicy::none,
                         ActionTargetRole::cooldown,
                         ActionValuePolicy::duration_or_integer},
    AutomationActionSpec{AutomationActionKind::reduce_cooldown_all,
                         "reduceCooldownAll", "Reduce all recoveries",
                         AutomationEffectDomain::cooldown_engine,
                         ActionSecondaryPolicy::none,
                         ActionTargetRole::cooldown,
                         ActionValuePolicy::duration_or_integer},
    AutomationActionSpec{AutomationActionKind::refund_cooldown,
                         "refundCooldown", "Refund cooldown",
                         AutomationEffectDomain::cooldown_engine,
                         ActionSecondaryPolicy::none,
                         ActionTargetRole::cooldown, ActionValuePolicy::none},
    AutomationActionSpec{AutomationActionKind::trigger_group, "triggerGroup",
                         "Trigger group",
                         AutomationEffectDomain::cooldown_engine,
                         ActionSecondaryPolicy::none,
                         ActionTargetRole::cooldown_group,
                         ActionValuePolicy::none},
    AutomationActionSpec{AutomationActionKind::reset_group, "resetGroup",
                         "Reset group",
                         AutomationEffectDomain::cooldown_engine,
                         ActionSecondaryPolicy::none,
                         ActionTargetRole::cooldown_group,
                         ActionValuePolicy::none},
    AutomationActionSpec{AutomationActionKind::create_timer, "createTimer",
                         "Create timer", AutomationEffectDomain::internal,
                         ActionSecondaryPolicy::none,
                         ActionTargetRole::timer_id,
                         ActionValuePolicy::duration_or_integer, true},
    AutomationActionSpec{AutomationActionKind::cancel_timer, "cancelTimer",
                         "Cancel timer", AutomationEffectDomain::internal,
                         ActionSecondaryPolicy::none,
                         ActionTargetRole::timer_id, ActionValuePolicy::none},
    AutomationActionSpec{
        AutomationActionKind::set_overlay_override, "setOverlayOverride",
        "Set overlay override", AutomationEffectDomain::overlay,
        ActionSecondaryPolicy::optional_duration, ActionTargetRole::panel,
        ActionValuePolicy::overlay_property, false, true, true, true},
    AutomationActionSpec{
        AutomationActionKind::clear_overlay_override, "clearOverlayOverride",
        "Clear overlay override", AutomationEffectDomain::overlay,
        ActionSecondaryPolicy::none, ActionTargetRole::override_name,
        ActionValuePolicy::none},
};
static_assert(automation_action_specs.size() == automation_action_kind_count);

[[nodiscard]] constexpr const AutomationActionSpec *
automation_action_spec(const AutomationActionKind kind) noexcept {
  for (const auto &spec : automation_action_specs)
    if (spec.kind == kind) return &spec;
  return nullptr;
}

[[nodiscard]] constexpr std::optional<std::string_view>
automation_action_name(const AutomationActionKind kind) noexcept {
  const auto *spec = automation_action_spec(kind);
  return spec == nullptr ? std::nullopt
                         : std::optional<std::string_view>(spec->wire_name);
}

[[nodiscard]] constexpr std::optional<AutomationActionKind>
parse_automation_action_name(const std::string_view name) noexcept {
  for (const auto &spec : automation_action_specs)
    if (spec.wire_name == name) return spec.kind;
  return std::nullopt;
}

struct AutomationAction {
  AutomationActionKind kind{AutomationActionKind::set_variable};
  std::string target;
  // Named presentation overrides can update several properties as one
  // session-local layer and can later be cleared explicitly by this name.
  std::string name;
  std::string property;
  // setOverlayOverride only: one item of the target panel; empty = the panel.
  std::string item_id{};
  // Absent when the action's spec does not accept that argument. Replaces the
  // old null-literal sentinel so illegal states are unrepresentable.
  std::optional<AutomationExpression> value;
  std::optional<AutomationExpression> secondary;
  bool repeating{};

  bool operator==(const AutomationAction &) const = default;
};

[[nodiscard]] constexpr AutomationEffectDomain
effect_domain(const AutomationActionKind kind) noexcept {
  const auto *spec = automation_action_spec(kind);
  return spec == nullptr ? AutomationEffectDomain::internal : spec->domain;
}

struct AutomationRule {
  std::string id;
  std::int32_t priority{};
  AutomationEventKind event{AutomationEventKind::input_pressed};
  std::string source_filter;
  AutomationExpression condition{.kind = ExpressionKind::literal,
                                 .operation = {},
                                 .reference = {},
                                 .literal = AutomationValue(true),
                                 .arguments = {}};
  std::vector<AutomationAction> actions;
  bool operator==(const AutomationRule &) const = default;
};

struct AutomationDefinition {
  std::string id;
  std::string name;
  bool enabled{true};
  std::vector<AutomationRule> rules;
  bool operator==(const AutomationDefinition &) const = default;
};

struct AutomationVariable {
  std::string id;
  AutomationValueType type{AutomationValueType::boolean};
  AutomationValue default_value{false};
  bool persistent{};
  bool operator==(const AutomationVariable &) const = default;
};

// The one rule for accepting a stored value into a persistent variable:
// runtime start, sibling/state application, and the state file share it.
[[nodiscard]] bool
automation_persistent_value_accepted(const AutomationVariable &variable,
                                     const AutomationValue &value) noexcept;

struct ContinuousInputSettings {
  double deadzone{0.15};
  double change_threshold{0.02};
  std::uint32_t max_events_per_second{60};
  bool operator==(const ContinuousInputSettings &) const = default;
};

struct AutomationProfile {
  std::uint32_t schema_version{1};
  std::string profile_id;
  ContinuousInputSettings continuous_input;
  std::vector<AutomationVariable> variables;
  std::vector<AutomationDefinition> automations;
  bool operator==(const AutomationProfile &) const = default;
};

// True when at least one enabled automation has a rule to evaluate: the one
// demand rule both daemons use to start an automation runtime.
[[nodiscard]] inline bool
has_enabled_automations(const AutomationProfile &profile) noexcept {
  return std::ranges::any_of(profile.automations, [](const auto &automation) {
    return automation.enabled && !automation.rules.empty();
  });
}

using AutomationError = DocumentError;
using AutomationProfileResult =
    std::variant<AutomationProfile, AutomationError>;
using AutomationValueParseResult =
    std::variant<AutomationValue, AutomationError>;
[[nodiscard]] AutomationValueParseResult
parse_automation_value_json(std::string_view source,
                            AutomationValueType expected_type);

[[nodiscard]] AutomationProfileResult
parse_automation_profile_json(std::string_view source,
                              const GameProfile &base_profile);
// For a bounded document already accepted by json::parse (including UTF-8).
[[nodiscard]] AutomationProfileResult parse_automation_profile_value(
    const json::Value &root, const GameProfile &base_profile);
[[nodiscard]] AutomationError
validate_automation_profile(const AutomationProfile &profile,
                            const GameProfile &base_profile);
[[nodiscard]] std::string
serialize_automation_profile_json(const AutomationProfile &profile);
using AutomationPersistentValues =
    std::map<std::string, AutomationValue, std::less<>>;
using AutomationStateResult =
    std::variant<AutomationPersistentValues, AutomationError>;
[[nodiscard]] AutomationStateResult
parse_automation_state_json(std::string_view source,
                            const AutomationProfile &profile);
[[nodiscard]] std::string
serialize_automation_state_json(const AutomationProfile &profile,
                                const AutomationPersistentValues &values);

struct AutomationTimer {
  std::string id;
  std::string owner_automation;
  TimePoint due{};
  Milliseconds interval{};
  bool repeating{};
};

struct EffectOrder {
  std::int32_t priority{};
  std::size_t automation_order{};
  std::size_t rule_order{};

  bool operator==(const EffectOrder &) const = default;
};

// Canonical winner ordering: highest priority wins, ties break by automation
// order, then rule order. Shared by runtime effects and overlay overrides so
// platforms cannot pick different winners for equal-priority rules.
[[nodiscard]] constexpr bool
effect_order_precedes(const EffectOrder &left,
                      const EffectOrder &right) noexcept {
  if (left.priority != right.priority)
    return left.priority > right.priority;
  if (left.automation_order != right.automation_order)
    return left.automation_order < right.automation_order;
  return left.rule_order < right.rule_order;
}

struct OverlayPropertyOverride {
  std::string name;
  std::string owner_automation;
  std::string panel_id;
  // Empty for panel scope; otherwise the targeted item of panel_id.
  std::string item_id{};
  std::string property;
  AutomationValue value;
  std::optional<TimePoint> expires_at;
  EffectOrder order{};

  bool operator==(const OverlayPropertyOverride &) const = default;
};

[[nodiscard]] inline bool overlay_override_precedes(
    const OverlayPropertyOverride &left,
    const OverlayPropertyOverride &right) noexcept {
  return effect_order_precedes(left.order, right.order);
}

// Canonical domain of presentation properties automation may override. One
// registry drives static validation (property name and value type), the
// daemon's effect validation and overlay application, and the editor's value
// type hints, so the bounds cannot drift between processes.
struct ResolvedPanelOverrides;

// Where an overlay property applies: the whole panel, one item, or both.
inline constexpr std::uint8_t overlay_scope_panel = 1U;
inline constexpr std::uint8_t overlay_scope_item = 2U;
[[nodiscard]] constexpr std::uint8_t
overlay_item_kind_bit(const OverlayItemKind kind) noexcept {
  return static_cast<std::uint8_t>(1U << static_cast<unsigned>(kind));
}
inline constexpr std::uint8_t overlay_all_item_kinds =
    overlay_item_kind_bit(OverlayItemKind::cooldown) |
    overlay_item_kind_bit(OverlayItemKind::timer) |
    overlay_item_kind_bit(OverlayItemKind::counter) |
    overlay_item_kind_bit(OverlayItemKind::indicator);

struct OverlayPropertySpec {
  std::string_view name;
  AutomationValueType type;
  // Inclusive numeric bounds for integer/number values (ignored otherwise).
  double min_value{};
  double max_value{};
  bool (*validate)(const OverlayPropertySpec &, const AutomationValue &) noexcept;
  // Panel-scope application; null for item-only properties.
  void (*apply)(ResolvedPanelOverrides &, const AutomationValue &);
  std::uint8_t scopes{overlay_scope_panel};
  // Item kinds an item-scoped value applies to.
  std::uint8_t item_kinds{};
  void (*apply_item)(ItemPresentationOverride &, const AutomationValue &){};
};

[[nodiscard]] constexpr bool
overlay_property_in_scope(const OverlayPropertySpec &spec,
                          const bool item_scope) noexcept {
  return (spec.scopes & (item_scope ? overlay_scope_item
                                    : overlay_scope_panel)) != 0U;
}

[[nodiscard]] const OverlayPropertySpec *
find_overlay_property(std::string_view name) noexcept;
[[nodiscard]] bool overlay_value_valid(const OverlayPropertySpec &spec,
                                       const AutomationValue &value) noexcept;

// Preferred literal editing type, derived from the canonical action policy.
// This is a hint, not validation: durations prefer duration over integer;
// absent/unconstrained values and unresolved variables have no hint.
[[nodiscard]] inline std::optional<AutomationValueType>
automation_action_value_type(const AutomationAction &action,
                             const AutomationProfile &profile,
                             const bool normalization_only = false) {
  const auto *spec = automation_action_spec(action.kind);
  if (spec == nullptr)
    return std::nullopt;
  switch (spec->value) {
  case ActionValuePolicy::match_variable:
  case ActionValuePolicy::match_numeric_variable: {
    const auto variable =
        std::ranges::find(profile.variables, action.target,
                          &AutomationVariable::id);
    return variable == profile.variables.end()
               ? std::nullopt
               : std::optional<AutomationValueType>(variable->type);
  }
  case ActionValuePolicy::string_key:
    return normalization_only ? std::nullopt
                              : std::optional(AutomationValueType::string);
  case ActionValuePolicy::duration_or_integer:
    return normalization_only ? std::nullopt
                              : std::optional(AutomationValueType::duration);
  case ActionValuePolicy::overlay_property: {
    const auto *property = find_overlay_property(action.property);
    return property == nullptr ? std::optional(AutomationValueType::integer)
                               : std::optional(property->type);
  }
  case ActionValuePolicy::none:
  case ActionValuePolicy::any:
    return std::nullopt;
  }
  return std::nullopt;
}

// One panel after its winning overrides: Panel-field properties are applied
// to `panel`; the session-level visibility and text layers are returned for
// the platform renderer.
struct ResolvedPanelOverrides {
  Panel panel;
  std::optional<bool> visible{};
  std::optional<std::string> text{};
  // Item-scoped winners, ordered by item ID.
  std::vector<ItemPresentationOverride> items{};
};
[[nodiscard]] ResolvedPanelOverrides
resolve_panel_overrides(const Panel &base,
                        std::span<const OverlayPropertyOverride> overrides);

struct AutomationEffect {
  AutomationActionKind kind{AutomationActionKind::set_variable};
  std::string automation_id;
  std::string target;
  std::string name;
  std::string property;
  std::string item_id{};
  TimePoint timestamp{};
  AutomationValue value;
  AutomationValue secondary;
  bool repeating{};
  EffectOrder order{};
};

// One well-formedness rule for action values, reduction/timer durations,
// and overlay values/optional expiry durations. Targets are session-owned.
[[nodiscard]] std::optional<AutomationError>
automation_effect_error(const AutomationEffect &effect);

// Only explicit external roots are replayable; derived/audit entries are not.
inline constexpr char automation_trace_root_phase[] = "root";

struct AutomationTraceEntry {
  std::uint64_t sequence{};
  Milliseconds timestamp{};
  std::string automation_id;
  std::string rule_id;
  std::string phase;
  std::string detail;
  std::optional<AutomationEvent> source_event;
};

struct AutomationEvaluationResult {
  bool committed{};
  std::vector<AutomationEffect> effects;
  std::vector<AutomationEvent> derived_events;
  std::vector<std::string> faulted_automations;
  std::optional<AutomationError> error;
  std::size_t node_evaluations{};
};

struct AutomationRuntimeSnapshot {
  std::map<std::string, AutomationValue, std::less<>> variables;
  std::vector<AutomationTimer> timers;
  std::vector<OverlayPropertyOverride> overlay_overrides;
  // Session-wide createTimer targets, including disabled rules/definitions.
  // Declaration survives cancellation/expiry; absent IDs are missing sources.
  std::vector<std::string> declared_timer_ids;
};

// What presentation reads when no evaluator runs: declared variable defaults
// and declared timer IDs, but no running timers and no overrides. Never starts
// an automation loop.
[[nodiscard]] AutomationRuntimeSnapshot
automation_default_snapshot(const AutomationProfile &profile);

// Session-scoped edge-detection state the daemon uses to emit ready and
// value-changed events (previous observations, last emission times). Owned by
// the runtime so a profile switch or session retirement resets it atomically
// with the evaluation state it describes.
struct AutomationSessionEdges {
  std::map<std::string, bool, std::less<>> cooldown_ready;
  std::map<std::string, bool, std::less<>> group_ready;
  std::map<std::string, double, std::less<>> input_values;
  std::map<std::string, TimePoint, std::less<>> input_emitted;
};

// An AutomationProfile the canonical validator accepted against its base
// GameProfile. The evaluator relies on the arity, operator and type
// invariants validate_automation_profile establishes, so this — never a raw
// AutomationProfile — is what AutomationRuntime accepts. Immutable after
// acceptance; validate() is the only way to construct one.
class ValidatedAutomationProfile {
public:
  using Result = std::variant<ValidatedAutomationProfile, AutomationError>;
  [[nodiscard]] static Result validate(AutomationProfile profile,
                                       const GameProfile &base);
  [[nodiscard]] const AutomationProfile &profile() const noexcept {
    return profile_;
  }
  bool operator==(const ValidatedAutomationProfile &) const = default;

private:
  explicit ValidatedAutomationProfile(AutomationProfile profile) noexcept
      : profile_(std::move(profile)) {}
  AutomationProfile profile_;
};

// Current runtime faults of one automation runtime: the last evaluation
// error, and one reason per automation that faulted during an event. Both
// daemons report these lines; they never block unrelated automations.
struct AutomationRuntimeHealth {
  // Set by the session when the canonical validator rejected a candidate.
  std::optional<std::string> rejected;
  std::optional<std::string> runtime_error;
  std::map<std::string, std::string, std::less<>> runtime_faults;

  // One line per current fault, prefixed with the session's profile source.
  [[nodiscard]] std::vector<std::string> lines(std::string_view source) const;
};

class AutomationRuntime {
public:
  using EffectValidator = std::function<bool(const AutomationEffect &)>;
  // The complete, prevalidated winning set is committed in one call so an
  // adapter can stage its own mutable state and publish atomically.
  using EffectCommitter =
      std::function<bool(std::span<const AutomationEffect>)>;

  // Clock injection permits deterministic budget tests; production uses the
  // monotonic clock. The reader is cooperative, not callback preemption.
  using ClockReader = std::function<TimePoint()>;
  AutomationRuntime(ValidatedAutomationProfile profile,
                    AutomationPersistentValues persistent_values = {},
                    CooldownGroupTopology cooldown_groups = {},
                    ClockReader clock = MonotonicClock::now);
  ~AutomationRuntime();

  [[nodiscard]] const AutomationProfile &profile() const noexcept;
  [[nodiscard]] std::span<const CooldownGroup> cooldown_groups() const noexcept;
  [[nodiscard]] const AutomationRuntimeSnapshot &snapshot() const noexcept;
  // The persistent variables' current values, derived from snapshot().
  [[nodiscard]] AutomationPersistentValues persistent_values() const;
  [[nodiscard]] AutomationSessionEdges &edges() noexcept;
  [[nodiscard]] const AutomationSessionEdges &edges() const noexcept;
  // Evaluates one root event as a transaction: every condition and value
  // expression reads the batch-start snapshot (not staged mid-batch state),
  // so two rules keyed on the same trigger race on equal footing and only
  // the committed winning set becomes observable. Effects commit in one
  // validator+committer call; an attributable error faults only the offending
  // automation, which is re-evaluated from the original snapshot. Global
  // exhaustion rejects the root without faulting automations. Checks are
  // cooperative, including immediately before the non-preemptible committer.
  // Cooldown events derived from trigger/reset actions use the shared
  // CooldownGroupTopology sets: the runtime does not own the cooldown engine,
  // so whether a charge was actually consumed is decided by the daemon-side
  // committer (its own trigger paths emit engine-derived events with real
  // origin tokens).
  [[nodiscard]] AutomationEvaluationResult
  process(const AutomationEvent &event, const EffectValidator &validator = {},
          const EffectCommitter &committer = {});
  // Pure collection of due timer firings, ordered oldest deadline first.
  // The firing is consumed only when process() commits its transaction:
  // unprocessed or rejected firings stay due for the next poll.
  [[nodiscard]] std::vector<AutomationEvent> due_timer_events(TimePoint now);
  // Time-based housekeeping in the spirit of cooldown recovery: override
  // expiry is a fact about the clock, not an automation decision, so it
  // happens outside the evaluation transaction. Returns whether anything
  // expired so callers can refresh overlays.
  [[nodiscard]] bool expire_overlay_overrides(TimePoint now);
  [[nodiscard]] std::optional<TimePoint> next_deadline() const noexcept;
  // Native sessions pass the engine's immutable topology handle; no second
  // group-definition copy or borrowed lifetime is held by the runtime.
  void reconfigure(ValidatedAutomationProfile profile,
                   CooldownGroupTopology cooldown_groups = {});
  void apply_persistent_values(const AutomationPersistentValues &values);
  void retry(std::string_view automation_id);
  // A value copy of the faults this runtime currently owns; reconfiguration,
  // retry and a fresh runtime in the owning session are its only lifecycle.
  [[nodiscard]] AutomationRuntimeHealth health() const;
  [[nodiscard]] bool persistent_dirty() const noexcept;
  void mark_persistent_saved() noexcept;
  [[nodiscard]] const std::vector<AutomationTraceEntry> &trace() const noexcept;
  [[nodiscard]] bool trace_enabled() const noexcept;
  void set_trace_enabled(bool enabled);
  void consume_trace(std::size_t count);
  void clear_trace();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};


struct AutomationReplayAssertion {
  std::string variable_id;
  AutomationValue expected;
};
struct AutomationReplayResult {
  bool passed{};
  std::size_t processed_events{};
  std::vector<std::string> failures;
  AutomationRuntimeSnapshot final_state;
};
[[nodiscard]] AutomationReplayResult replay_automation_events(
    const ValidatedAutomationProfile &profile,
    std::span<const AutomationEvent> events,
    std::span<const AutomationReplayAssertion> assertions = {},
    std::span<const CooldownGroup> cooldown_groups = {});
// Records one external event as a replay root (for example, simulator input).
[[nodiscard]] AutomationTraceEntry
make_automation_root_trace_entry(const AutomationEvent &event,
                                std::uint64_t sequence);
[[nodiscard]] std::string
export_layerc_trace(std::span<const AutomationTraceEntry> entries);
// One entry exactly as export_layerc_trace writes it, so a transport can
// size trace chunks without re-encoding whole documents.
void append_layerc_trace_entry(std::string &output,
                               const AutomationTraceEntry &entry);
using AutomationTraceParseResult =
    std::variant<std::vector<AutomationEvent>, AutomationError>;
[[nodiscard]] AutomationTraceParseResult
parse_layerc_trace(std::string_view source);
// Optional caller allocation budget; not a change to the trace format limits.
[[nodiscard]] AutomationTraceParseResult
parse_layerc_trace(std::string_view source, std::size_t max_json_values);

} // namespace layerc::core
