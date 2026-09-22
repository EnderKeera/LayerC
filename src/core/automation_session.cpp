// Shared per-session automation lifecycle policy. See automation_session.hpp.

#include "layerc/core/automation_session.hpp"

#include "layerc/core/cooldown_engine.hpp"

#include <algorithm>
#include <map>
#include <ranges>

namespace layerc::core {

bool automation_overlay_effect_valid(
    const AutomationEffect &effect) noexcept {
  const auto *spec = find_overlay_property(effect.property);
  if (spec == nullptr || !overlay_value_valid(*spec, effect.value))
    return false;
  if (std::holds_alternative<std::nullptr_t>(effect.secondary.storage))
    return true;
  return as_duration(effect.secondary).has_value();
}

bool validate_automation_effect(
    const AutomationEffect &effect, const std::span<const Panel> panels,
    const std::span<const CooldownDefinition> cooldowns,
    const std::span<const CooldownGroup> groups) {
  switch (effect_domain(effect.kind)) {
  case AutomationEffectDomain::overlay:
    if (effect.kind != AutomationActionKind::set_overlay_override) return true;
    return automation_overlay_effect_valid(effect) &&
           std::ranges::any_of(panels, [&](const Panel &panel) {
             return panel.id == effect.target;
           });
  case AutomationEffectDomain::cooldown_engine:
    return validate_automation_cooldown_effect(effect, cooldowns, groups);
  case AutomationEffectDomain::internal:
    return true;
  default:
    return false;
  }
}

bool validate_automation_cooldown_effect(
    const AutomationEffect &effect,
    const std::span<const CooldownDefinition> cooldowns,
    const std::span<const CooldownGroup> groups) {
  if (effect_domain(effect.kind) != AutomationEffectDomain::cooldown_engine)
    return false;
  if (effect.kind == AutomationActionKind::reduce_cooldown_next ||
      effect.kind == AutomationActionKind::reduce_cooldown_all) {
    if (!as_duration(effect.value).has_value()) return false;
  }
  if (effect.kind == AutomationActionKind::trigger_group ||
      effect.kind == AutomationActionKind::reset_group)
    return std::ranges::any_of(groups, [&](const CooldownGroup &group) {
      return group.id == effect.target;
    });
  return std::ranges::any_of(
      cooldowns, [&](const CooldownDefinition &cooldown) {
        return cooldown.id == effect.target;
      });
}

bool commit_automation_cooldown_effects(
    CooldownEngine &engine, const std::span<const AutomationEffect> effects,
    const std::span<const CooldownDefinition> cooldowns,
    const std::span<const CooldownGroup> groups, const TimePoint now,
    const std::uint64_t origin_token) {
  // Keep the live engine unchanged if validation or application rejects any
  // effect. The caller supplies the definitions used to configure this engine.
  for (const auto &effect : effects) {
    if (effect_domain(effect.kind) !=
        AutomationEffectDomain::cooldown_engine)
      continue;
    if (!validate_automation_cooldown_effect(effect, cooldowns, groups))
      return false;
  }
  auto staged = engine;
  for (const auto &effect : effects) {
    if (effect_domain(effect.kind) !=
        AutomationEffectDomain::cooldown_engine)
      continue;
    if (!apply_automation_cooldown_effect(staged, effect, now, origin_token))
      return false;
  }
  engine = std::move(staged);
  return true;
}

std::optional<TimePoint> next_automation_deadline(
    const AutomationRuntime &runtime, const std::span<const CooldownState> cooldowns,
    const std::optional<TimePoint> checkpoint) {
  auto next = checkpoint;
  const auto include = [&](const std::optional<TimePoint> due) {
    if (due && (!next || *due < *next)) next = due;
  };
  include(runtime.next_timer_due());
  for (const auto &item : runtime.snapshot().overlay_overrides)
    include(item.expires_at);
  for (const auto &state : cooldowns)
    include(state.next_charge_at);
  return next;
}

ReadyTransitions poll_automation_ready_transitions(
    AutomationSessionEdges &edges,
    const std::span<const CooldownState> snapshot,
    const std::span<const CooldownGroup> groups) {
  ReadyTransitions ready;
  std::map<std::string, bool, std::less<>> cooldown_ready;
  for (const auto &state : snapshot) {
    const bool is_ready = state.available_charges == state.max_charges;
    cooldown_ready[state.id] = is_ready;
    const auto previous = edges.cooldown_ready.find(state.id);
    if (previous != edges.cooldown_ready.end() && !previous->second &&
        is_ready)
      ready.cooldowns.push_back(state.id);
  }
  // Groups read from this poll's local map (assigned to edges only at the
  // end): one snapshot in, one consistent verdict out. The maps are sized
  // by the snapshot (bounded by the profile's cooldown/group counts), so
  // the per-poll allocation stays proportional on the 2 ms budget.
  std::map<std::string, bool, std::less<>> group_ready;
  for (const auto &group : groups) {
    const bool is_ready =
        !group.cooldown_ids.empty() &&
        std::ranges::all_of(group.cooldown_ids, [&](const std::string &id) {
          const auto found = cooldown_ready.find(id);
          return found != cooldown_ready.end() && found->second;
        });
    group_ready[group.id] = is_ready;
    const auto previous = edges.group_ready.find(group.id);
    if (previous != edges.group_ready.end() && !previous->second && is_ready)
      ready.groups.push_back(group.id);
  }
  edges.cooldown_ready = std::move(cooldown_ready);
  edges.group_ready = std::move(group_ready);
  return ready;
}

} // namespace layerc::core
