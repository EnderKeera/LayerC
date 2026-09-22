#include "automation_runtime_impl.hpp"

#include "layerc/core/utf8.hpp"

#include <algorithm>
#include <utility>

namespace layerc::core {

void AutomationRuntime::Impl::consume_timer_firing(const std::string_view id,
                                                   const TimePoint now) {
  const auto timer = std::ranges::find(state.timers, id, &AutomationTimer::id);
  if (timer == state.timers.end() || timer->due > now)
    return; // cancelled, replaced, or a firing that is not yet due
  if (timer->repeating)
    timer->due = now + timer->interval;
  else
    state.timers.erase(timer);
}

void AutomationRuntime::Impl::make_trace_room() {
  if (trace_entries.size() == 2048)
    trace_entries.erase(trace_entries.begin(), trace_entries.begin() + 256);
}

void AutomationRuntime::Impl::add_source_trace(const AutomationEvent &event) {
  if (!trace_enabled)
    return;
  make_trace_room();
  AutomationTraceEntry entry;
  entry.sequence = ++trace_sequence;
  entry.timestamp = std::chrono::duration_cast<Milliseconds>(
      event.timestamp.time_since_epoch());
  entry.phase = "source";
  entry.detail = std::string(automation_event_name(event.kind));
  entry.source_event = event;
  trace_entries.push_back(std::move(entry));
}

void AutomationRuntime::Impl::add_trace(const AutomationEvent &event,
                                        std::string automation,
                                        std::string rule, std::string phase,
                                        std::string detail) {
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
      !is_valid_utf8(root.source_id) ||
      root.fields.size() > max_automation_collection_entries ||
      !std::ranges::all_of(root.fields, [](const auto &field) {
        return field.first.size() <= max_automation_string_bytes &&
               is_valid_utf8(field.first) &&
               automation_value_valid(field.second);
      })) {
    result.error = {"$event", "source event exceeds automation bounds"};
    return result;
  }
  // Delivering a timer firing consumes it—repeating timers rearm from the
  // event timestamp, one-shot timers retire—before evaluation begins. The
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
