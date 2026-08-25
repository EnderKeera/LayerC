#include "automation_json.hpp"
#include "automation_ops.hpp"
#include "layerc/core/automation.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/live_edit.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_set>

namespace layerc::core {
namespace {
using detail::checked_integer_binary;
using detail::duration_result;
using detail::number;
using detail::truthy;
std::string effect_key(const AutomationEffect &effect) {
  switch (effect.kind) {
  case AutomationActionKind::set_variable:
  case AutomationActionKind::increment_variable:
  case AutomationActionKind::append_list:
  case AutomationActionKind::set_map_entry:
  case AutomationActionKind::erase_map_entry:
    return "v:" + effect.target;
  case AutomationActionKind::trigger_cooldown:
  case AutomationActionKind::reset_cooldown:
  case AutomationActionKind::reduce_cooldown_next:
  case AutomationActionKind::reduce_cooldown_all:
  case AutomationActionKind::refund_cooldown:
    return "c:" + effect.target;
  case AutomationActionKind::trigger_group:
  case AutomationActionKind::reset_group:
    return "g:" + effect.target;
  case AutomationActionKind::create_timer:
  case AutomationActionKind::cancel_timer:
    return "t:" + effect.target;
  case AutomationActionKind::set_overlay_override:
    return "o:" + effect.name + ":" + effect.property;
  case AutomationActionKind::clear_overlay_override:
    return "o:" + effect.target + ":*";
  }
  return effect.target;
}
bool effect_precedes(const AutomationEffect &left,
                     const AutomationEffect &right) {
  if (left.priority != right.priority)
    return left.priority > right.priority;
  if (left.automation_order != right.automation_order)
    return left.automation_order < right.automation_order;
  return left.rule_order < right.rule_order;
}
// Defense in depth for hand-assembled profiles: parsing validates these
// contracts, but the evaluator indexes argument vectors directly, so an
// expression with an unknown operator or out-of-range arity must never
// reach eval().
bool expression_within_contract(const AutomationExpression &expression,
                                const std::size_t depth) {
  if (depth > max_automation_ast_depth)
    return false;
  switch (expression.kind) {
  case ExpressionKind::literal:
  case ExpressionKind::variable:
  case ExpressionKind::event_field:
    return true;
  case ExpressionKind::index:
    if (expression.arguments.size() != 2U)
      return false;
    break;
  case ExpressionKind::unary:
  case ExpressionKind::binary:
  case ExpressionKind::call: {
    const auto *spec = detail::find_automation_op(expression.operation);
    if (spec == nullptr || spec->kind != expression.kind)
      return false;
    if (expression.arguments.size() < spec->min_args ||
        expression.arguments.size() > spec->max_args)
      return false;
    break;
  }
  }
  return std::ranges::all_of(expression.arguments, [&](const auto &child) {
    return expression_within_contract(child, depth + 1U);
  });
}
bool expressions_within_contract(const AutomationDefinition &automation) {
  return std::ranges::all_of(automation.rules, [](const auto &rule) {
    if (!expression_within_contract(rule.condition, 0))
      return false;
    return std::ranges::all_of(rule.actions, [](const auto &action) {
      return expression_within_contract(action.value, 0) &&
             expression_within_contract(action.secondary, 0);
    });
  });
}
} // namespace

struct AutomationRuntime::Impl {
  AutomationProfile profile;
  std::vector<CooldownGroup> cooldown_groups;
  AutomationRuntimeSnapshot state;
  AutomationSessionEdges edges;
  std::set<std::string, std::less<>> faulted;
  bool persistent_dirty{};
  bool trace_enabled{};
  std::uint64_t trace_sequence{};
  std::vector<AutomationTraceEntry> trace_entries;

  Impl(AutomationProfile source, AutomationPersistentValues persistent,
       const std::span<const CooldownGroup> groups)
      : profile(std::move(source)),
        cooldown_groups(groups.begin(), groups.end()) {
    for (const auto &variable : profile.variables) {
      auto value = variable.default_value;
      if (variable.persistent) {
        const auto found = persistent.find(variable.id);
        if (found != persistent.end() &&
            value_type(found->second) == variable.type &&
            automation_value_valid(found->second))
          value = found->second;
        state.persistent_values[variable.id] = value;
      }
      state.variables[variable.id] = std::move(value);
    }
    // Fail closed on out-of-contract expressions instead of evaluating them:
    // the faulted set keeps the session diagnosable and retry/reload clears
    // it once the profile is fixed.
    for (const auto &automation : profile.automations)
      if (!expressions_within_contract(automation))
        faulted.insert(automation.id);
  }
  void consume_timer_firing(const std::string_view id, const TimePoint now) {
    const auto timer =
        std::ranges::find(state.timers, id, &AutomationTimer::id);
    if (timer == state.timers.end() || timer->due > now)
      return; // cancelled, replaced, or a firing that is not yet due
    if (timer->repeating)
      timer->due = now + timer->interval;
    else
      state.timers.erase(timer);
  }
  void make_trace_room() {
    if (trace_entries.size() == 2048)
      trace_entries.erase(trace_entries.begin(), trace_entries.begin() + 256);
  }
  void add_source_trace(const AutomationEvent &event) {
    if (!trace_enabled)
      return;
    make_trace_room();
    AutomationTraceEntry entry;
    entry.sequence = ++trace_sequence;
    entry.timestamp = std::chrono::duration_cast<Milliseconds>(
        event.timestamp.time_since_epoch());
    entry.phase = "source";
    entry.detail = std::string(detail::automation_event_name(event.kind));
    entry.source_event = event;
    trace_entries.push_back(std::move(entry));
  }
  void add_trace(const AutomationEvent &event, std::string automation,
                 std::string rule, std::string phase, std::string detail) {
    if (!trace_enabled)
      return;
    make_trace_room();
    AutomationTraceEntry entry;
    entry.sequence = ++trace_sequence;
    entry.timestamp = std::chrono::duration_cast<Milliseconds>(
        event.timestamp.time_since_epoch());
    entry.automation_id = std::move(automation);
    entry.rule_id = std::move(rule);
    entry.phase = std::move(phase);
    entry.detail = std::move(detail);
    trace_entries.push_back(std::move(entry));
  }
  std::optional<AutomationValue> eval(const AutomationExpression &expression,
                                      const AutomationEvent &event,
                                      const AutomationRuntimeSnapshot &snapshot,
                                      std::size_t &evaluations,
                                      AutomationError &error) const {
    if (++evaluations > max_automation_node_evaluations) {
      error = {"$runtime", "root event exceeded 8192 node evaluations"};
      return std::nullopt;
    }
    switch (expression.kind) {
    case ExpressionKind::literal:
      return expression.literal;
    case ExpressionKind::variable: {
      const auto found = snapshot.variables.find(expression.reference);
      if (found == snapshot.variables.end()) {
        error = {"$runtime", "unknown variable " + expression.reference};
        return std::nullopt;
      }
      return found->second;
    }
    case ExpressionKind::event_field: {
      if (expression.reference == "ageMs") {
        const auto recorded = event.fields.find("ageMs");
        if (recorded != event.fields.end())
          return recorded->second;
        return AutomationValue(DurationValue{Milliseconds::zero()});
      }
      if (expression.reference == "sourceId")
        return AutomationValue(event.source_id);
      const auto found = event.fields.find(expression.reference);
      return found == event.fields.end() ? AutomationValue{} : found->second;
    }
    case ExpressionKind::index: {
      auto target =
          eval(expression.arguments[0], event, snapshot, evaluations, error);
      auto key =
          eval(expression.arguments[1], event, snapshot, evaluations, error);
      if (!target || !key)
        return std::nullopt;
      if (const auto *list = std::get_if<AutomationList>(&target->storage)) {
        const auto *index = std::get_if<std::int64_t>(&key->storage);
        if (!index || *index < 0 ||
            static_cast<std::size_t>(*index) >= list->size())
          return AutomationValue{};
        return (*list)[static_cast<std::size_t>(*index)];
      }
      if (const auto *map = std::get_if<AutomationMap>(&target->storage)) {
        const auto *name = std::get_if<std::string>(&key->storage);
        if (!name)
          return AutomationValue{};
        const auto found = map->find(*name);
        return found == map->end() ? AutomationValue{} : found->second;
      }
      error = {"$runtime", "index target is not a list or map"};
      return std::nullopt;
    }
    default:
      break;
    }
    std::vector<AutomationValue> args;
    args.reserve(expression.arguments.size());
    for (const auto &argument : expression.arguments) {
      auto value = eval(argument, event, snapshot, evaluations, error);
      if (!value)
        return std::nullopt;
      args.push_back(std::move(*value));
    }
    const auto *spec = detail::find_automation_op(expression.operation);
    if (spec == nullptr) {
      error = {"$runtime", "unsupported expression operation"};
      return std::nullopt;
    }
    AutomationValue evaluated;
    std::string message;
    if (!spec->evaluate(*spec, args, evaluated, message)) {
      error = {"$runtime", message};
      return std::nullopt;
    }
    return evaluated;
  }
  std::vector<std::string>
  propagated_cooldowns(const std::span<const std::string> seeds) const {
    std::vector<std::string> result(seeds.begin(), seeds.end());
    std::set<std::string, std::less<>> seen(result.begin(), result.end());
    for (std::size_t index{}; index < result.size(); ++index) {
      for (const auto &group : cooldown_groups) {
        if (group.mode != CooldownGroupMode::trigger_propagation ||
            std::ranges::find(group.cooldown_ids, result[index]) ==
                group.cooldown_ids.end())
          continue;
        for (const auto &member : group.cooldown_ids)
          if (seen.insert(member).second)
            result.push_back(member);
      }
    }
    return result;
  }

  std::vector<std::string> group_cooldowns(const std::string_view group_id,
                                           const bool propagate) const {
    const auto group =
        std::ranges::find(cooldown_groups, group_id, &CooldownGroup::id);
    if (group == cooldown_groups.end())
      return {};
    return propagate ? propagated_cooldowns(group->cooldown_ids)
                     : group->cooldown_ids;
  }

  std::vector<std::string>
  reset_cooldowns(const std::string_view cooldown_id) const {
    for (const auto &group : cooldown_groups)
      if (group.mode == CooldownGroupMode::shared_pool &&
          std::ranges::find(group.cooldown_ids, cooldown_id) !=
              group.cooldown_ids.end())
        return group.cooldown_ids;
    return {std::string(cooldown_id)};
  }

  bool apply_internal(const AutomationEffect &effect, TimePoint now,
                      AutomationRuntimeSnapshot &target, AutomationError &error,
                      std::vector<AutomationEvent> &derived) const {
    auto variable = target.variables.find(effect.target);
    const auto changed = [&](const AutomationValue &before,
                             const AutomationValue &after) {
      if (before == after)
        return;
      AutomationEvent event;
      event.kind = AutomationEventKind::variable_changed;
      event.timestamp = now;
      event.source_id = effect.target;
      event.fields.emplace("old", before);
      event.fields.emplace("value", after);
      derived.push_back(std::move(event));
    };
    switch (effect.kind) {
    case AutomationActionKind::set_variable:
      if (variable == target.variables.end() ||
          !automation_value_valid(effect.value)) {
        error = {"$runtime", "invalid variable assignment"};
        return false;
      }
      if (value_type(variable->second) != value_type(effect.value)) {
        error = {"$runtime", "variable assignment changes its declared type"};
        return false;
      }
      {
        auto before = variable->second;
        variable->second = effect.value;
        changed(before, variable->second);
      }
      return true;
    case AutomationActionKind::increment_variable: {
      if (variable == target.variables.end()) {
        error = {"$runtime", "unknown variable"};
        return false;
      }
      const auto before = variable->second;
      if (const auto *integer_value =
              std::get_if<std::int64_t>(&before.storage)) {
        const auto *right = std::get_if<std::int64_t>(&effect.value.storage);
        const auto sum =
            right == nullptr
                ? std::optional<std::int64_t>{}
                : checked_integer_binary(*integer_value, *right, "add");
        if (!sum.has_value()) {
          error = {"$runtime", "integer increment overflow"};
          return false;
        }
        variable->second = AutomationValue(*sum);
      } else if (const auto *number_value =
                     std::get_if<double>(&before.storage)) {
        const auto right = number(effect.value);
        if (!right.has_value() || !std::isfinite(*number_value + *right)) {
          error = {"$runtime", "number increment is not finite"};
          return false;
        }
        variable->second = AutomationValue(*number_value + *right);
      } else if (const auto *duration_value =
                     std::get_if<DurationValue>(&before.storage)) {
        const auto *right = std::get_if<DurationValue>(&effect.value.storage);
        const auto sum =
            right == nullptr
                ? std::optional<AutomationValue>{}
                : duration_result(
                      static_cast<double>(duration_value->value.count()) +
                      static_cast<double>(right->value.count()));
        if (!sum.has_value()) {
          error = {"$runtime", "duration increment is out of range"};
          return false;
        }
        variable->second = *sum;
      } else if (const auto *confidence_value =
                     std::get_if<ConfidenceValue>(&before.storage)) {
        const auto *right = std::get_if<ConfidenceValue>(&effect.value.storage);
        const double sum =
            right == nullptr ? -1.0 : confidence_value->value + right->value;
        if (!std::isfinite(sum) || sum < 0.0 || sum > 1.0) {
          error = {"$runtime", "confidence increment is out of range"};
          return false;
        }
        variable->second = AutomationValue(ConfidenceValue{sum});
      } else {
        error = {"$runtime", "increment requires numeric values"};
        return false;
      }
      changed(before, variable->second);
      return true;
    }
    case AutomationActionKind::append_list: {
      if (variable == target.variables.end()) {
        error = {"$runtime", "unknown variable"};
        return false;
      }
      auto *list = std::get_if<AutomationList>(&variable->second.storage);
      if (!list || list->size() >= max_automation_collection_entries) {
        error = {"$runtime", "list is full or target is not a list"};
        return false;
      }
      auto before = variable->second;
      list->push_back(effect.value);
      changed(before, variable->second);
      return true;
    }
    case AutomationActionKind::set_map_entry: {
      if (variable == target.variables.end()) {
        error = {"$runtime", "unknown variable"};
        return false;
      }
      auto *map = std::get_if<AutomationMap>(&variable->second.storage);
      const auto *key = std::get_if<std::string>(&effect.secondary.storage);
      if (!map || !key ||
          (map->size() >= max_automation_collection_entries &&
           !map->contains(*key))) {
        error = {"$runtime", "map is full or map key is invalid"};
        return false;
      }
      auto before = variable->second;
      (*map)[*key] = effect.value;
      changed(before, variable->second);
      return true;
    }
    case AutomationActionKind::erase_map_entry: {
      if (variable == target.variables.end()) {
        error = {"$runtime", "unknown variable"};
        return false;
      }
      auto *map = std::get_if<AutomationMap>(&variable->second.storage);
      const auto *key = std::get_if<std::string>(&effect.value.storage);
      if (!map || !key) {
        error = {"$runtime", "eraseMapEntry requires a string key"};
        return false;
      }
      auto before = variable->second;
      map->erase(*key);
      changed(before, variable->second);
      return true;
    }
    case AutomationActionKind::create_timer: {
      const auto interval = as_duration(effect.value);
      if (!interval || *interval < min_automation_timer_interval) {
        error = {"$runtime", "timer interval must be at least 50 ms"};
        return false;
      }
      auto found =
          std::ranges::find(target.timers, effect.target, &AutomationTimer::id);
      if (found == target.timers.end()) {
        if (target.timers.size() >= max_automation_timers) {
          error = {"$runtime", "session timer limit reached"};
          return false;
        }
        target.timers.push_back({effect.target, effect.automation_id,
                                 now + *interval, *interval, effect.repeating});
      } else
        *found = {effect.target, effect.automation_id, now + *interval,
                  *interval, effect.repeating};
      return true;
    }
    case AutomationActionKind::cancel_timer:
      std::erase_if(target.timers, [&](const auto &timer) {
        return timer.id == effect.target;
      });
      return true;
    case AutomationActionKind::set_overlay_override: {
      std::optional<TimePoint> expiry;
      if (const auto value = as_duration(effect.secondary);
          value && *value > Milliseconds::zero())
        expiry = now + *value;
      std::erase_if(target.overlay_overrides, [&](const auto &item) {
        return item.name == effect.name && item.property == effect.property;
      });
      target.overlay_overrides.push_back(
          {.name = effect.name,
           .owner_automation = effect.automation_id,
           .panel_id = effect.target,
           .property = effect.property,
           .value = effect.value,
           .expires_at = expiry,
           .priority = effect.priority,
           .automation_order = effect.automation_order,
           .rule_order = effect.rule_order});
      return true;
    }
    case AutomationActionKind::clear_overlay_override:
      std::erase_if(target.overlay_overrides, [&](const auto &item) {
        return item.name == effect.target;
      });
      return true;
    case AutomationActionKind::trigger_cooldown: {
      // Closure semantics (deliberate): the runtime does not own the
      // CooldownEngine, so intra-batch cooldown_triggered events expand the
      // trigger-propagation closure from the profile's cooldown groups.
      // Whether a charge is actually consumed is decided by the daemon-side
      // committer against the real engine; the daemon's own trigger paths
      // emit engine-derived events with real origin tokens. A rule keyed on
      // cooldown_triggered inside the triggering batch therefore observes
      // "the trigger action was issued", not "a charge was consumed".
      const std::array<std::string, 1> seed{effect.target};
      for (const auto &id : propagated_cooldowns(seed))
        derived.push_back({.kind = AutomationEventKind::cooldown_triggered,
                           .timestamp = now,
                           .source_id = id,
                           .origin_token = 0,
                           .fields = {}});
      return true;
    }
    case AutomationActionKind::reset_cooldown:
      for (const auto &id : reset_cooldowns(effect.target))
        derived.push_back({.kind = AutomationEventKind::cooldown_ready,
                           .timestamp = now,
                           .source_id = id,
                           .origin_token = 0,
                           .fields = {}});
      return true;
    case AutomationActionKind::trigger_group:
      derived.push_back({.kind = AutomationEventKind::group_triggered,
                         .timestamp = now,
                         .source_id = effect.target,
                         .origin_token = 0,
                         .fields = {}});
      for (const auto &id : group_cooldowns(effect.target, true))
        derived.push_back({.kind = AutomationEventKind::cooldown_triggered,
                           .timestamp = now,
                           .source_id = id,
                           .origin_token = 0,
                           .fields = {}});
      return true;
    case AutomationActionKind::reset_group:
      derived.push_back({.kind = AutomationEventKind::group_ready,
                         .timestamp = now,
                         .source_id = effect.target,
                         .origin_token = 0,
                         .fields = {}});
      for (const auto &id : group_cooldowns(effect.target, false))
        derived.push_back({.kind = AutomationEventKind::cooldown_ready,
                           .timestamp = now,
                           .source_id = id,
                           .origin_token = 0,
                           .fields = {}});
      return true;
    default:
      return true;
    }
  }

  // One full evaluation attempt over the root event. Returns the automation
  // to fault and restart without when the attempt failed transactionally;
  // returns nullopt when result is final (committed, rejected, or capped).
  // node_evaluations and faulted_automations accumulate across attempts and
  // are owned by the caller.
  std::optional<std::string>
  evaluate(const AutomationEvent &root, const EffectValidator &validator,
           const EffectCommitter &committer, const TimePoint deadline,
           AutomationEvaluationResult &result, AutomationError &fault);
};
AutomationRuntime::AutomationRuntime(
    AutomationProfile profile, AutomationPersistentValues persistent,
    const std::span<const CooldownGroup> cooldown_groups)
    : impl_(std::make_unique<Impl>(std::move(profile), std::move(persistent),
                                   cooldown_groups)) {}
AutomationRuntime::~AutomationRuntime() = default;
const AutomationProfile &AutomationRuntime::profile() const noexcept {
  return impl_->profile;
}
std::span<const CooldownGroup>
AutomationRuntime::cooldown_groups() const noexcept {
  return impl_->cooldown_groups;
}
bool AutomationRuntime::active() const noexcept {
  return std::ranges::any_of(impl_->profile.automations, [&](const auto &a) {
    return a.enabled && !impl_->faulted.contains(a.id);
  });
}
const AutomationRuntimeSnapshot &AutomationRuntime::snapshot() const noexcept {
  return impl_->state;
}
AutomationSessionEdges &AutomationRuntime::edges() noexcept {
  return impl_->edges;
}
const AutomationSessionEdges &AutomationRuntime::edges() const noexcept {
  return impl_->edges;
}
AutomationEvaluationResult
AutomationRuntime::process(const AutomationEvent &root,
                           const EffectValidator &validator,
                           const EffectCommitter &committer) {
  AutomationEvaluationResult result;
  if (root.source_id.size() > max_automation_string_bytes ||
      !detail::is_valid_utf8(root.source_id) ||
      root.fields.size() > max_automation_collection_entries ||
      !std::ranges::all_of(root.fields, [](const auto &field) {
        return field.first.size() <= max_automation_string_bytes &&
               detail::is_valid_utf8(field.first) &&
               automation_value_valid(field.second);
      })) {
    result.error = {"$event", "source event exceeds automation bounds"};
    return result;
  }
  // Delivering a timer firing consumes it — repeating timers rearm from the
  // event timestamp, one-shot timers retire — before evaluation begins. The
  // firing is a fact about time, not an automation decision: consuming it
  // at delivery means a faulted or rejected batch neither loses the timer
  // nor double-fires it on the next poll.
  if (root.kind == AutomationEventKind::timer_fired)
    impl_->consume_timer_firing(root.source_id, root.timestamp);
  // One deadline spans the whole fault-retry cascade: restarting the
  // evaluation used to grant each recursive retry a fresh budget, so a
  // chain of faulted automations could block the daemon loop for up to
  // max_automation_rules x 2 ms on a single root event.
  const auto deadline = MonotonicClock::now() + automation_execution_budget;
  std::optional<AutomationError> last_fault;
  for (;;) {
    const auto trace_sequence_start = impl_->trace_sequence;
    AutomationError fault;
    const auto restart_with =
        impl_->evaluate(root, validator, committer, deadline, result, fault);
    if (!restart_with.has_value()) {
      if (!result.error.has_value() && last_fault.has_value())
        result.error = *last_fault;
      return result;
    }
    // Rewind this attempt's source-phase traces so the restart re-records
    // them; condition/error traces stay as the fault's audit trail.
    std::erase_if(impl_->trace_entries, [&](const auto &entry) {
      return entry.sequence > trace_sequence_start && entry.phase == "source";
    });
    impl_->faulted.insert(*restart_with);
    if (std::ranges::find(result.faulted_automations, *restart_with) ==
        result.faulted_automations.end())
      result.faulted_automations.push_back(*restart_with);
    last_fault = fault;
  }
}

std::optional<std::string> AutomationRuntime::Impl::evaluate(
    const AutomationEvent &root, const EffectValidator &validator,
    const EffectCommitter &committer, const TimePoint deadline,
    AutomationEvaluationResult &result, AutomationError &fault) {
  result.effects.clear();
  result.derived_events.clear();
  result.committed = false;
  result.error.reset();
  // Batch isolation (deliberate): every condition and value expression in
  // this batch reads root_snapshot — the state as it was when the root event
  // arrived — not the staged mid-batch state. Two rules keyed on the same
  // trigger therefore race on equal footing, and a losing rule's staged
  // write cannot change whether a later rule fires; only the committed
  // winning set becomes observable. Evaluating staged state instead would
  // make rule outcomes order-dependent inside one batch.
  const auto root_snapshot = state;
  // The staged copy is bounded by the caps (<=64 variables, <=32 timers,
  // <=64 collection entries per value) and accepted per benchmark v23:
  // transactional rollback requires a private working state.
  auto staged = root_snapshot;
  std::vector<AutomationEvent> queue{root};
  // Rule identity as (automation order << 32) | rule order: the match loop
  // already tracks both indices, so dedup needs no per-rule key allocation.
  const auto fired_key = [](const std::size_t ai, const std::size_t ri) {
    return (static_cast<std::uint64_t>(ai) << 32U) |
           static_cast<std::uint64_t>(ri);
  };
  std::set<std::uint64_t> fired;
  std::map<std::string, AutomationEffect, std::less<>> winners;
  std::size_t queue_index{};
  while (queue_index < queue.size()) {
    if (queue_index >= max_automation_derived_events + 1U) {
      // The transaction cannot complete within the derived-event cap: fail
      // the whole batch without committing partial effects.
      result.error =
          AutomationError{"$runtime", "root event exceeded 32 derived events"};
      return std::nullopt;
    }
    const auto event = queue[queue_index++];
    add_source_trace(event);
    struct Match {
      const AutomationDefinition *automation;
      const AutomationRule *rule;
      std::size_t ai;
      std::size_t ri;
    };
    std::vector<Match> matches;
    for (std::size_t ai{}; ai < profile.automations.size(); ++ai) {
      const auto &automation = profile.automations[ai];
      if (!automation.enabled || faulted.contains(automation.id))
        continue;
      for (std::size_t ri{}; ri < automation.rules.size(); ++ri) {
        const auto &rule = automation.rules[ri];
        if (rule.event == event.kind &&
            (rule.source_filter.empty() ||
             rule.source_filter == event.source_id) &&
            !fired.contains(fired_key(ai, ri)))
          matches.push_back({&automation, &rule, ai, ri});
      }
    }
    std::ranges::stable_sort(matches, [](const Match &a, const Match &b) {
      if (a.rule->priority != b.rule->priority)
        return a.rule->priority > b.rule->priority;
      if (a.ai != b.ai)
        return a.ai < b.ai;
      return a.ri < b.ri;
    });
    for (const auto &match : matches) {
      if (MonotonicClock::now() > deadline) {
        const AutomationError timeout{"$runtime",
                                      "automation execution exceeded 2 ms"};
        add_trace(event, match.automation->id, match.rule->id, "error",
                  timeout.message);
        fault = timeout;
        return match.automation->id;
      }
      AutomationError error;
      auto condition = eval(match.rule->condition, event, root_snapshot,
                            result.node_evaluations, error);
      if (!condition) {
        add_trace(event, match.automation->id, match.rule->id, "error",
                  error.message);
        fault = error;
        return match.automation->id;
      }
      if (!truthy(*condition)) {
        add_trace(event, match.automation->id, match.rule->id, "condition",
                  "false");
        continue;
      }
      fired.insert(fired_key(match.ai, match.ri));
      add_trace(event, match.automation->id, match.rule->id, "condition",
                "true");
      bool rule_fault{};
      std::vector<AutomationEffect> candidate;
      for (const auto &action : match.rule->actions) {
        AutomationEffect effect;
        effect.kind = action.kind;
        effect.automation_id = match.automation->id;
        effect.rule_id = match.rule->id;
        effect.target = action.target;
        effect.name = action.name;
        effect.property = action.property;
        effect.timestamp = event.timestamp;
        effect.repeating = action.repeating;
        effect.priority = match.rule->priority;
        effect.automation_order = match.ai;
        effect.rule_order = match.ri;
        const bool needs_value = action_accepts_value(action.kind);
        if (needs_value) {
          auto value = eval(action.value, event, root_snapshot,
                            result.node_evaluations, error);
          if (!value) {
            rule_fault = true;
            break;
          }
          effect.value = std::move(*value);
        }
        if (action.secondary.kind != ExpressionKind::literal ||
            !std::holds_alternative<std::nullptr_t>(
                action.secondary.literal.storage)) {
          auto secondary = eval(action.secondary, event, root_snapshot,
                                result.node_evaluations, error);
          if (!secondary) {
            rule_fault = true;
            break;
          }
          effect.secondary = std::move(*secondary);
        }
        candidate.push_back(std::move(effect));
      }
      if (rule_fault) {
        add_trace(event, match.automation->id, match.rule->id, "error",
                  error.message);
        fault = error;
        return match.automation->id;
      }
      auto candidate_state = staged;
      auto candidate_winners = winners;
      std::vector<AutomationEvent> candidate_derived;
      for (auto &effect : candidate) {
        const auto key = effect_key(effect);
        const auto found = candidate_winners.find(key);
        if (found != candidate_winners.end() &&
            !effect_precedes(effect, found->second))
          continue;
        std::vector<AutomationEvent> derived;
        if (!apply_internal(effect, event.timestamp, candidate_state, error,
                            derived)) {
          rule_fault = true;
          break;
        }
        candidate_winners.insert_or_assign(key, effect);
        for (auto &next : derived) {
          next.origin_token = root.origin_token;
          candidate_derived.push_back(std::move(next));
        }
      }
      if (rule_fault) {
        add_trace(event, match.automation->id, match.rule->id, "error",
                  error.message);
        fault = error;
        return match.automation->id;
      }
      const auto derived_count = queue.size() - 1U;
      if (candidate_derived.size() >
          max_automation_derived_events - derived_count) {
        const AutomationError cap_fault{
            "$runtime", "root event exceeded 32 derived events"};
        add_trace(event, match.automation->id, match.rule->id, "error",
                  cap_fault.message);
        fault = cap_fault;
        return match.automation->id;
      }
      staged = std::move(candidate_state);
      winners = std::move(candidate_winners);
      for (auto &next : candidate_derived)
        queue.push_back(std::move(next));
    }
  }
  for (const auto &[key, effect] : winners) {
    (void)key;
    if (faulted.contains(effect.automation_id))
      continue;
    if (validator && !validator(effect)) {
      const AutomationError rejection{
          "$runtime", "transaction validation rejected an action"};
      fault = rejection;
      return effect.automation_id;
    }
    result.effects.push_back(effect);
  }
  std::ranges::sort(result.effects, effect_precedes);
  AutomationRuntimeSnapshot committed_state = root_snapshot;
  for (const auto &effect : result.effects) {
    std::vector<AutomationEvent> ignored;
    AutomationError apply_error;
    if (!apply_internal(effect, effect.timestamp, committed_state, apply_error,
                        ignored)) {
      fault = apply_error;
      return effect.automation_id;
    }
  }
  if (committer && !committer(result.effects)) {
    result.error = AutomationError{
        "$runtime", "transaction commit rejected the action batch"};
    return std::nullopt;
  }
  state = std::move(committed_state);
  for (const auto &variable : profile.variables)
    if (variable.persistent) {
      const auto found = state.variables.find(variable.id);
      if (found == state.variables.end())
        continue;
      auto &saved = state.persistent_values[variable.id];
      if (saved != found->second) {
        saved = found->second;
        persistent_dirty = true;
      }
    }
  result.derived_events.assign(queue.begin() + 1, queue.end());
  result.committed = true;
  return std::nullopt;
}
std::vector<AutomationEvent>
AutomationRuntime::due_timer_events(TimePoint now) {
  // Pure collection, ordered oldest deadline first: the firing is consumed
  // when the event is delivered to process(), so a collected but
  // unprocessed event leaves the timer due and the next poll re-collects it
  // (at-least-once delivery) instead of losing it at collection time.
  std::vector<std::pair<TimePoint, const AutomationTimer *>> due;
  for (const auto &timer : impl_->state.timers) {
    if (timer.due > now || due.size() >= max_automation_derived_events)
      continue;
    due.emplace_back(timer.due, &timer);
  }
  std::ranges::sort(due, {},
                    &std::pair<TimePoint, const AutomationTimer *>::first);
  std::vector<AutomationEvent> events;
  events.reserve(due.size());
  for (const auto &[deadline, timer] : due) {
    AutomationEvent event;
    event.kind = AutomationEventKind::timer_fired;
    event.timestamp = now;
    event.source_id = timer->id;
    event.fields.emplace(
        "lateBy",
        AutomationValue(DurationValue{std::max(
            Milliseconds::zero(),
            std::chrono::duration_cast<Milliseconds>(now - deadline))}));
    events.push_back(std::move(event));
  }
  return events;
}
bool AutomationRuntime::expire_overlay_overrides(const TimePoint now) {
  const auto before = impl_->state.overlay_overrides.size();
  std::erase_if(impl_->state.overlay_overrides, [&](const auto &item) {
    return item.expires_at && *item.expires_at <= now;
  });
  return impl_->state.overlay_overrides.size() != before;
}
std::optional<TimePoint> AutomationRuntime::next_timer_due() const noexcept {
  std::optional<TimePoint> result;
  for (const auto &timer : impl_->state.timers)
    if (!result || timer.due < *result)
      result = timer.due;
  for (const auto &item : impl_->state.overlay_overrides)
    if (item.expires_at && (!result || *item.expires_at < *result))
      result = item.expires_at;
  return result;
}
void AutomationRuntime::reconfigure(
    AutomationProfile profile,
    const std::span<const CooldownGroup> cooldown_groups) {
  AutomationRuntimeSnapshot next;
  for (const auto &variable : profile.variables) {
    auto value = variable.default_value;
    const auto old = impl_->state.variables.find(variable.id);
    if (old != impl_->state.variables.end() &&
        value_type(old->second) == variable.type)
      value = old->second;
    next.variables[variable.id] = value;
    if (variable.persistent)
      next.persistent_values[variable.id] = value;
  }
  std::set<std::string, std::less<>> retained;
  for (const auto &automation : profile.automations) {
    const auto previous = std::ranges::find(
        impl_->profile.automations, automation.id, &AutomationDefinition::id);
    if (previous != impl_->profile.automations.end() && *previous == automation)
      retained.insert(automation.id);
  }
  for (const auto &timer : impl_->state.timers)
    if (retained.contains(timer.owner_automation))
      next.timers.push_back(timer);
  for (const auto &item : impl_->state.overlay_overrides)
    if (retained.contains(item.owner_automation))
      next.overlay_overrides.push_back(item);
  impl_->profile = std::move(profile);
  impl_->cooldown_groups.assign(cooldown_groups.begin(), cooldown_groups.end());
  impl_->state = std::move(next);
  // A profile switch invalidates the daemon's previous ready/value
  // observations: re-baseline the edge monitors with the new profile.
  impl_->edges = {};
  impl_->faulted.clear();
}
void AutomationRuntime::apply_persistent_values(
    const AutomationPersistentValues &values) {
  for (const auto &variable : impl_->profile.variables) {
    if (!variable.persistent)
      continue;
    const auto found = values.find(variable.id);
    if (found == values.end() || value_type(found->second) != variable.type ||
        !automation_value_valid(found->second))
      continue;
    impl_->state.variables[variable.id] = found->second;
    impl_->state.persistent_values[variable.id] = found->second;
  }
  // Deliberately does not clear persistent_dirty: values pushed in from a
  // sibling session are not a durable write of THIS runtime's snapshot, and
  // clearing here would drop this session's unsaved-edit marker (silent data
  // loss). Only mark_persistent_saved() after a durable write may clear it;
  // a stale true flag just re-pushes identical values on the next sync.
}
void AutomationRuntime::retry(std::string_view id) {
  impl_->faulted.erase(std::string(id));
}
void AutomationRuntime::clear_session_state() noexcept {
  impl_->state.timers.clear();
  impl_->state.overlay_overrides.clear();
  impl_->edges = {};
  for (const auto &variable : impl_->profile.variables) {
    auto value = variable.default_value;
    if (variable.persistent) {
      const auto found = impl_->state.persistent_values.find(variable.id);
      if (found != impl_->state.persistent_values.end())
        value = found->second;
    }
    impl_->state.variables[variable.id] = std::move(value);
  }
  impl_->faulted.clear();
}
bool AutomationRuntime::persistent_dirty() const noexcept {
  return impl_->persistent_dirty;
}
void AutomationRuntime::mark_persistent_saved() noexcept {
  impl_->persistent_dirty = false;
}
std::string AutomationRuntime::persistent_state_json() const {
  return serialize_automation_state_json(impl_->profile,
                                         impl_->state.persistent_values);
}

const std::vector<AutomationTraceEntry> &
AutomationRuntime::trace() const noexcept {
  return impl_->trace_entries;
}
void AutomationRuntime::set_trace_enabled(bool enabled) {
  impl_->trace_enabled = enabled;
  if (!enabled)
    impl_->trace_entries.clear();
}
void AutomationRuntime::consume_trace(const std::size_t count) {
  const auto consumed = std::min(count, impl_->trace_entries.size());
  impl_->trace_entries.erase(impl_->trace_entries.begin(),
                             impl_->trace_entries.begin() +
                                 static_cast<std::ptrdiff_t>(consumed));
}
void AutomationRuntime::clear_trace() { impl_->trace_entries.clear(); }
} // namespace layerc::core
