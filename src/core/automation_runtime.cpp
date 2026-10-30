#include "automation_runtime_impl.hpp"

#include "layerc/core/utf8.hpp"

#include <algorithm>
#include <utility>

namespace layerc::core {

void AutomationRuntime::Impl::make_trace_room() {
  if (trace_entries.size() == max_automation_trace_entries)
    trace_entries.erase(
        trace_entries.begin(),
        trace_entries.begin() +
            static_cast<std::ptrdiff_t>(automation_trace_trim_entries));
}

std::optional<AutomationTraceEntry>
AutomationRuntime::Impl::begin_trace(const AutomationEvent &event) {
  if (!trace_enabled)
    return std::nullopt;
  make_trace_room();
  AutomationTraceEntry entry;
  entry.sequence = ++trace_sequence;
  entry.timestamp = std::chrono::duration_cast<Milliseconds>(
      event.timestamp.time_since_epoch());
  return entry;
}

void AutomationRuntime::Impl::add_event_trace(const AutomationEvent &event,
                                              std::string phase) {
  auto entry = begin_trace(event);
  if (!entry.has_value())
    return;
  entry->phase = std::move(phase);
  entry->detail = std::string(automation_event_name(event.kind));
  entry->source_event = event;
  trace_entries.push_back(std::move(*entry));
}

void AutomationRuntime::Impl::add_trace(const AutomationEvent &event,
                                        std::string automation,
                                        std::string rule, std::string phase,
                                        std::string detail) {
  auto entry = begin_trace(event);
  if (!entry.has_value())
    return;
  entry->automation_id = std::move(automation);
  entry->rule_id = std::move(rule);
  entry->phase = std::move(phase);
  entry->detail = std::move(detail);
  trace_entries.push_back(std::move(*entry));
}

AutomationRuntime::AutomationRuntime(
    AutomationProfile profile, AutomationPersistentValues persistent,
    CooldownGroupTopology cooldown_groups, ClockReader clock)
    : impl_(std::make_unique<Impl>(std::move(profile), std::move(persistent),
                                   std::move(cooldown_groups), std::move(clock))) {}
AutomationRuntime::~AutomationRuntime() = default;
const AutomationProfile &AutomationRuntime::profile() const noexcept {
  return impl_->profile;
}
std::span<const CooldownGroup>
AutomationRuntime::cooldown_groups() const noexcept {
  return impl_->groups.groups();
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
  const auto field_in_bounds = [](const auto &field) {
    return field.first.size() <= max_automation_string_bytes &&
           is_valid_utf8(field.first) && automation_value_valid(field.second);
  };
  if (root.source_id.size() > max_automation_string_bytes ||
      !is_valid_utf8(root.source_id) ||
      root.fields.size() > max_automation_collection_entries ||
      !std::ranges::all_of(root.fields, field_in_bounds)) {
    result.error = {"$event", "source event exceeds automation bounds"};
    return result;
  }
  // One budget spans every attributable-fault retry. Root provenance belongs
  // here, not in the cascade evaluator: children are audit-only on every
  // attempt.
  const auto deadline =
      automation_time_after(impl_->clock(), automation_execution_budget);
  impl_->add_event_trace(root, automation_trace_root_phase);
  if (!deadline) {
    result.error = {"$runtime",
                    "execution deadline is outside the clock range"};
    return result;
  }
  EvaluationBudget budget{*deadline, result.node_evaluations, impl_->clock};
  std::optional<AutomationError> last_fault;
  for (;;) {
    const auto outcome =
        impl_->evaluate(root, validator, committer, budget, result);
    if (const auto *failure = std::get_if<AutomationFault>(&outcome)) {
      impl_->faulted.insert(failure->automation_id);
      if (std::ranges::find(result.faulted_automations,
                            failure->automation_id) ==
          result.faulted_automations.end())
        result.faulted_automations.push_back(failure->automation_id);
      last_fault = failure->error;
      continue;
    }
    if (const auto *exhausted = std::get_if<BudgetExhausted>(&outcome))
      result.error = exhausted->error;
    else if (const auto *rejected = std::get_if<AttemptRejected>(&outcome))
      result.error = rejected->error;
    else if (last_fault)
      result.error = *last_fault;
    if (!result.committed) {
      result.effects.clear();
      result.derived_events.clear();
    }
    return result;
  }
}

std::vector<AutomationEvent>
AutomationRuntime::due_timer_events(TimePoint now) {
  // Pure collection, ordered oldest deadline first. A firing is consumed only
  // when process() commits its transaction; unprocessed or rejected firings
  // stay due for the next poll (at-least-once delivery).
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
        automation_event_field::late_by,
        AutomationValue(DurationValue{std::max(
            Milliseconds::zero(),
            std::chrono::duration_cast<Milliseconds>(now - deadline))}));
    events.push_back(std::move(event));
  }
  return events;
}
AutomationRuntimeSnapshot
automation_default_snapshot(const AutomationProfile &profile) {
  AutomationRuntimeSnapshot snapshot;
  for (const auto &variable : profile.variables)
    snapshot.variables.emplace(variable.id, variable.default_value);
  for (const auto &automation : profile.automations)
    for (const auto &rule : automation.rules)
      for (const auto &action : rule.actions)
        if (action.kind == AutomationActionKind::create_timer)
          snapshot.declared_timer_ids.push_back(action.target);
  std::ranges::sort(snapshot.declared_timer_ids);
  const auto duplicate = std::ranges::unique(snapshot.declared_timer_ids);
  snapshot.declared_timer_ids.erase(duplicate.begin(), duplicate.end());
  return snapshot;
}

bool AutomationRuntime::expire_overlay_overrides(const TimePoint now) {
  const auto before = impl_->state.overlay_overrides.size();
  std::erase_if(impl_->state.overlay_overrides, [&](const auto &item) {
    return item.expires_at && *item.expires_at <= now;
  });
  return impl_->state.overlay_overrides.size() != before;
}
std::optional<TimePoint> AutomationRuntime::next_deadline() const noexcept {
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
    CooldownGroupTopology cooldown_groups) {
  auto next = automation_default_snapshot(profile);
  for (const auto &variable : profile.variables) {
    const auto old = impl_->state.variables.find(variable.id);
    if (old != impl_->state.variables.end() &&
        value_type(old->second) == variable.type)
      next.variables[variable.id] = old->second;
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
  impl_->groups = std::move(cooldown_groups);
  impl_->state = std::move(next);
  // A profile switch invalidates the daemon's previous ready/value
  // observations: re-baseline the edge monitors with the new profile.
  impl_->edges = {};
  impl_->faulted.clear();
}
AutomationPersistentValues AutomationRuntime::persistent_values() const {
  AutomationPersistentValues values;
  for (const auto &variable : impl_->profile.variables) {
    if (!variable.persistent)
      continue;
    if (const auto found = impl_->state.variables.find(variable.id);
        found != impl_->state.variables.end())
      values.emplace(variable.id, found->second);
  }
  return values;
}
void AutomationRuntime::apply_persistent_values(
    const AutomationPersistentValues &values) {
  impl_->apply_persistent(values);
  // Deliberately does not clear persistent_dirty: values pushed in from a
  // sibling session are not a durable write of THIS runtime's snapshot, and
  // clearing here would drop this session's unsaved-edit marker (silent data
  // loss). Only mark_persistent_saved() after a durable write may clear it;
  // a stale true flag just re-pushes identical values on the next sync.
}
void AutomationRuntime::retry(std::string_view id) {
  impl_->faulted.erase(std::string(id));
}
bool AutomationRuntime::persistent_dirty() const noexcept {
  return impl_->persistent_dirty;
}
void AutomationRuntime::mark_persistent_saved() noexcept {
  impl_->persistent_dirty = false;
}
const std::vector<AutomationTraceEntry> &
AutomationRuntime::trace() const noexcept {
  return impl_->trace_entries;
}
bool AutomationRuntime::trace_enabled() const noexcept {
  return impl_->trace_enabled;
}
void AutomationRuntime::set_trace_enabled(bool enabled) {
  impl_->trace_enabled = enabled;
  if (!enabled)
    std::vector<AutomationTraceEntry>{}.swap(impl_->trace_entries);
}
void AutomationRuntime::consume_trace(const std::size_t count) {
  const auto consumed = std::min(count, impl_->trace_entries.size());
  impl_->trace_entries.erase(impl_->trace_entries.begin(),
                             impl_->trace_entries.begin() +
                                 static_cast<std::ptrdiff_t>(consumed));
}
void AutomationRuntime::clear_trace() { impl_->trace_entries.clear(); }
} // namespace layerc::core
