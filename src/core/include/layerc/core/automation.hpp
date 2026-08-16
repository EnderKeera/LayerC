#pragma once

#include "layerc/core/types.hpp"

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

inline constexpr std::size_t max_automation_json_bytes = 256U * 1024U;
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
[[nodiscard]] std::string automation_value_json(const AutomationValue &);
// Interprets a duration-typed or non-negative integer value as a duration;
// anything else is not a duration.
[[nodiscard]] std::optional<Milliseconds>
as_duration(const AutomationValue &value) noexcept;

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
  std::string operation;
  std::string reference;
  AutomationValue literal;
  std::vector<AutomationExpression> arguments;

  bool operator==(const AutomationExpression &) const = default;
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

struct AutomationAction {
  AutomationActionKind kind{AutomationActionKind::set_variable};
  std::string target;
  // Named presentation overrides can update several properties as one
  // session-local layer and can later be cleared explicitly by this name.
  std::string name;
  std::string property;
  AutomationExpression value;
  AutomationExpression secondary;
  bool repeating{};

  bool operator==(const AutomationAction &) const = default;
};

// True for actions whose primary value expression is evaluated before the
// transaction commits (parser, validator, serializer, and runtime agree on
// this one classifier).
[[nodiscard]] constexpr bool
action_accepts_value(const AutomationActionKind kind) noexcept {
  switch (kind) {
  case AutomationActionKind::set_variable:
  case AutomationActionKind::increment_variable:
  case AutomationActionKind::append_list:
  case AutomationActionKind::set_map_entry:
  case AutomationActionKind::erase_map_entry:
  case AutomationActionKind::reduce_cooldown_next:
  case AutomationActionKind::reduce_cooldown_all:
  case AutomationActionKind::create_timer:
  case AutomationActionKind::set_overlay_override:
    return true;
  case AutomationActionKind::trigger_cooldown:
  case AutomationActionKind::reset_cooldown:
  case AutomationActionKind::refund_cooldown:
  case AutomationActionKind::trigger_group:
  case AutomationActionKind::reset_group:
  case AutomationActionKind::cancel_timer:
  case AutomationActionKind::clear_overlay_override:
    return false;
  }
  return false;
}

// Where an action's side effects land. Internal actions mutate only runtime
// state; cooldown actions commit through the engine adapter; overlay actions
// publish presentation overrides.
enum class AutomationEffectDomain : std::uint8_t {
  internal,
  cooldown_engine,
  overlay,
};

[[nodiscard]] constexpr AutomationEffectDomain
effect_domain(const AutomationActionKind kind) noexcept {
  switch (kind) {
  case AutomationActionKind::trigger_cooldown:
  case AutomationActionKind::reset_cooldown:
  case AutomationActionKind::reduce_cooldown_next:
  case AutomationActionKind::reduce_cooldown_all:
  case AutomationActionKind::refund_cooldown:
  case AutomationActionKind::trigger_group:
  case AutomationActionKind::reset_group:
    return AutomationEffectDomain::cooldown_engine;
  case AutomationActionKind::set_overlay_override:
  case AutomationActionKind::clear_overlay_override:
    return AutomationEffectDomain::overlay;
  case AutomationActionKind::set_variable:
  case AutomationActionKind::increment_variable:
  case AutomationActionKind::append_list:
  case AutomationActionKind::set_map_entry:
  case AutomationActionKind::erase_map_entry:
  case AutomationActionKind::create_timer:
  case AutomationActionKind::cancel_timer:
    return AutomationEffectDomain::internal;
  }
  return AutomationEffectDomain::internal;
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

struct AutomationError {
  std::string path;
  std::string message;
};
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

struct OverlayPropertyOverride {
  std::string name;
  std::string owner_automation;
  std::string panel_id;
  std::string property;
  AutomationValue value;
  std::optional<TimePoint> expires_at;
  std::int32_t priority{};
  std::size_t automation_order{};
  std::size_t rule_order{};

  bool operator==(const OverlayPropertyOverride &) const = default;
};

// Canonical domain of presentation properties automation may override. One
// registry drives static validation (property name and value type), the
// daemon's effect validation and overlay application, and the editor's value
// type hints, so the bounds cannot drift between processes.
struct OverlayPropertySpec {
  std::string_view name;
  AutomationValueType type;
  // Inclusive numeric bounds for integer/number values (ignored otherwise).
  double min_value{};
  double max_value{};
  // Session-level presentation layers rather than Panel fields; the daemon's
  // overlay renderer owns their application.
  bool panel_visibility{};
  bool panel_text{};
  // Allowed string values (layout); entries empty for free bounded text.
  std::array<std::string_view, 3> string_choices{};
  void (*apply)(Panel &, const AutomationValue &) noexcept;
};

[[nodiscard]] const OverlayPropertySpec *
find_overlay_property(std::string_view name) noexcept;
[[nodiscard]] bool overlay_value_valid(const OverlayPropertySpec &spec,
                                       const AutomationValue &value) noexcept;

struct AutomationEffect {
  AutomationActionKind kind{AutomationActionKind::set_variable};
  std::string automation_id;
  std::string rule_id;
  std::string target;
  std::string name;
  std::string property;
  TimePoint timestamp{};
  AutomationValue value;
  AutomationValue secondary;
  bool repeating{};
  std::int32_t priority{};
  std::size_t automation_order{};
  std::size_t rule_order{};
};

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
  std::map<std::string, AutomationValue, std::less<>> persistent_values;
  std::vector<AutomationTimer> timers;
  std::vector<OverlayPropertyOverride> overlay_overrides;
};

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

class AutomationRuntime {
public:
  using EffectValidator = std::function<bool(const AutomationEffect &)>;
  // The complete, prevalidated winning set is committed in one call so an
  // adapter can stage its own mutable state and publish atomically.
  using EffectCommitter =
      std::function<bool(std::span<const AutomationEffect>)>;

  AutomationRuntime(AutomationProfile profile,
                    AutomationPersistentValues persistent_values = {},
                    std::span<const CooldownGroup> cooldown_groups = {});
  ~AutomationRuntime();

  [[nodiscard]] const AutomationProfile &profile() const noexcept;
  [[nodiscard]] std::span<const CooldownGroup> cooldown_groups() const noexcept;
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] const AutomationRuntimeSnapshot &snapshot() const noexcept;
  [[nodiscard]] AutomationSessionEdges &edges() noexcept;
  [[nodiscard]] const AutomationSessionEdges &edges() const noexcept;
  [[nodiscard]] AutomationEvaluationResult
  process(const AutomationEvent &event, const EffectValidator &validator = {},
          const EffectCommitter &committer = {});
  [[nodiscard]] std::vector<AutomationEvent> due_timer_events(TimePoint now);
  [[nodiscard]] std::optional<TimePoint> next_timer_due() const noexcept;
  void reconfigure(AutomationProfile profile,
                   std::span<const CooldownGroup> cooldown_groups = {});
  void apply_persistent_values(const AutomationPersistentValues &values);
  void retry(std::string_view automation_id);
  void clear_session_state() noexcept;
  [[nodiscard]] bool persistent_dirty() const noexcept;
  void mark_persistent_saved() noexcept;
  [[nodiscard]] std::string persistent_state_json() const;
  [[nodiscard]] const std::vector<AutomationTraceEntry> &trace() const noexcept;
  void set_trace_enabled(bool enabled);
  void consume_trace(std::size_t count);
  void clear_trace();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class CooldownEngine;
[[nodiscard]] bool
apply_automation_cooldown_effect(CooldownEngine &cooldowns,
                                 const AutomationEffect &effect, TimePoint now,
                                 std::uint64_t origin_token);

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
    const AutomationProfile &profile, std::span<const AutomationEvent> events,
    std::span<const AutomationReplayAssertion> assertions = {},
    std::span<const CooldownGroup> cooldown_groups = {});
[[nodiscard]] std::string
export_layerc_trace(std::span<const AutomationTraceEntry> entries);
struct LiveMessage;
[[nodiscard]] std::size_t
live_trace_prefix_count(const LiveMessage &base_message,
                        std::span<const AutomationTraceEntry> entries);
using AutomationTraceParseResult =
    std::variant<std::vector<AutomationEvent>, AutomationError>;
[[nodiscard]] AutomationTraceParseResult
parse_layerc_trace(std::string_view source);

} // namespace layerc::core