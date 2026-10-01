#include "layerc/core/cooldown_engine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ranges>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace layerc::core {
namespace {

// Parallel recovery derives the public next-charge time from the earliest
// pending deadline; every derivation site shares this rule so the empty
// (fully charged) case maps to nullopt exactly once.
[[nodiscard]] std::optional<TimePoint>
earliest_deadline(const std::vector<TimePoint> &deadlines) noexcept {
  if (deadlines.empty())
    return std::nullopt;
  return *std::ranges::min_element(deadlines);
}

} // namespace

Milliseconds cooldown_remaining_ms(
    const std::optional<TimePoint> &next_charge_at, const TimePoint now) noexcept {
  if (!next_charge_at.has_value()) return Milliseconds::zero();
  return std::max(
      Milliseconds::zero(),
      std::chrono::duration_cast<Milliseconds>(*next_charge_at - now));
}

std::string cooldown_status_text(const CooldownDefinition &definition,
                                 const CooldownState &state,
                                 const TimePoint now) {
  std::string label = definition.name;
  if (state.next_charge_at.has_value()) {
    const auto remaining = cooldown_remaining_ms(state.next_charge_at, now);
    const auto tenths = (remaining.count() + 99) / 100;
    label += "  " + std::to_string(tenths / 10) + "." +
             std::to_string(tenths % 10) + "s";
  } else {
    label += "  READY";
  }
  if (state.max_charges > 1)
    label += "  [" + std::to_string(state.available_charges) + "]";
  return label;
}

const CooldownState *cooldown_state_at(
    const std::span<const CooldownState> states, const std::size_t index,
    const std::string_view cooldown_id) noexcept {
  if (index < states.size() && states[index].id == cooldown_id)
    return &states[index];
  const auto found = std::ranges::find(states, cooldown_id, &CooldownState::id);
  return found == states.end() ? nullptr : &*found;
}

double cooldown_progress(const CooldownDefinition &definition,
                         const CooldownState &state,
                         const TimePoint now) noexcept {
  if (!state.next_charge_at.has_value()) return 1.0;
  const auto duration = state.recharge_duration.value_or(definition.duration);
  if (duration <= Milliseconds::zero()) return 0.0;
  const auto remaining = cooldown_remaining_ms(state.next_charge_at, now);
  return std::clamp(static_cast<double>(remaining.count()) /
                        static_cast<double>(duration.count()),
                    0.0, 1.0);
}

CooldownEngine::CooldownEngine(
    const std::span<const CooldownDefinition> definitions,
    const std::span<const CooldownGroup> groups)
    : groups_(groups.begin(), groups.end()) {
  entries_.reserve(definitions.size());
  order_.reserve(definitions.size());
  owner_slots_.reserve(definitions.size());
  // Every pool has at least one member, so there can be no more owners than
  // public cooldowns. This sentinel is outside every assigned slot.
  const auto unassigned = definitions.size();

  for (const auto &source : definitions) {
    if (source.id.empty())
      throw std::invalid_argument("cooldown id must not be empty");
    if (source.duration <= Milliseconds::zero())
      throw std::invalid_argument("cooldown duration must be positive");
    if (source.max_charges == 0)
      throw std::invalid_argument("cooldown max_charges must be positive");

    if (!owner_slots_.emplace(source.id, unassigned).second)
      throw std::invalid_argument("duplicate cooldown id: " + source.id);
    order_.push_back(source.id);
  }

  std::unordered_set<std::string> group_ids;
  for (const auto &group : groups_) {
    if (group.id.empty() || !group_ids.insert(group.id).second)
      throw std::invalid_argument("duplicate or empty cooldown group id");
    if (group.cooldown_ids.empty())
      throw std::invalid_argument("cooldown group must contain a member");

    std::unordered_set<std::string> members;
    for (const auto &id : group.cooldown_ids) {
      if (!owner_slots_.contains(id) || !members.insert(id).second)
        throw std::invalid_argument("invalid cooldown group member: " + id);
    }
    if (group.mode != CooldownGroupMode::shared_pool)
      continue;
    if (group.duration <= Milliseconds::zero() || group.max_charges == 0)
      throw std::invalid_argument("shared pool requires duration and charges");

    const auto slot = entries_.size();
    entries_.push_back({.definition = {.kind = CooldownOwnerKind::shared_pool,
                                      .id = group.id,
                                      .duration = group.duration,
                                      .max_charges = group.max_charges,
                                      .retrigger = group.retrigger,
                                      .recovery = group.recovery},
                        .available_charges = group.max_charges,
                        .next_charge_at = std::nullopt,
                        .recovery_deadlines = {}});
    for (const auto &id : group.cooldown_ids) {
      auto &member_slot = owner_slots_.at(id);
      if (member_slot != unassigned)
        throw std::invalid_argument(
            "cooldown cannot belong to multiple shared pools: " + id);
      member_slot = slot;
    }
  }
  // Allocate standalone owners only after pool membership is known. Pooled
  // members keep their public identity, but never acquire dormant state.
  for (const auto &source : definitions) {
    auto &slot = owner_slots_.at(source.id);
    if (slot != unassigned)
      continue;
    slot = entries_.size();
    entries_.push_back({.definition = {.kind = CooldownOwnerKind::cooldown,
                                      .id = source.id,
                                      .duration = source.duration,
                                      .max_charges = source.max_charges,
                                      .retrigger = source.retrigger,
                                      .recovery = source.recovery},
                        .available_charges = source.max_charges,
                        .next_charge_at = std::nullopt,
                        .recovery_deadlines = {}});
  }
}

CooldownEngine::Entry *
CooldownEngine::entry_for(const std::string_view cooldown_id) {
  return const_cast<Entry *>(std::as_const(*this).entry_for(cooldown_id));
}

const CooldownEngine::Entry *
CooldownEngine::entry_for(const std::string_view cooldown_id) const {
  const auto found = owner_slots_.find(cooldown_id);
  return found == owner_slots_.end() ? nullptr : &entries_[found->second];
}

const CooldownOwnerDefinition *
CooldownEngine::owner_definition(const std::string_view cooldown_id) const {
  const auto *entry = entry_for(cooldown_id);
  return entry == nullptr ? nullptr : &entry->definition;
}

bool CooldownEngine::trigger_entry(Entry &entry, const TimePoint now) {
  update_entry(entry, now);

  const auto recovery = entry.definition.recovery;
  if (recovery == RecoveryMode::single && entry.next_charge_at) {
    if (entry.definition.retrigger != RetriggerPolicy::restart)
      return false;
    entry.next_charge_at = now + entry.definition.duration;
    return true;
  }

  if (entry.available_charges > 0) {
    --entry.available_charges;
    if (recovery == RecoveryMode::parallel) {
      entry.recovery_deadlines.push_back(now + entry.definition.duration);
      entry.next_charge_at = earliest_deadline(entry.recovery_deadlines);
    } else if (!entry.next_charge_at) {
      entry.next_charge_at = now + entry.definition.duration;
    }
    return true;
  }

  if (entry.definition.retrigger != RetriggerPolicy::restart)
    return false;
  if (recovery == RecoveryMode::parallel && !entry.recovery_deadlines.empty()) {
    const auto earliest = earliest_deadline(entry.recovery_deadlines);
    const auto shift = now + entry.definition.duration - *earliest;
    for (auto &deadline : entry.recovery_deadlines)
      deadline += shift;
    entry.next_charge_at = earliest_deadline(entry.recovery_deadlines);
  } else {
    entry.next_charge_at = now + entry.definition.duration;
  }
  return true;
}

std::vector<std::string> CooldownEngine::trigger_many_with_propagation(
    const std::span<const std::string> cooldown_ids, const TimePoint now,
    const std::uint64_t origin_token) {
  std::vector<std::string> pending(cooldown_ids.begin(), cooldown_ids.end());
  std::vector<std::string> triggered;
  std::unordered_set<Entry *> visited_entries;
  std::unordered_set<std::string> expanded_members;

  for (std::size_t index{}; index < pending.size(); ++index) {
    // Copy the work item: expanding pending below may reallocate the queue
    // and invalidate any reference into it (overlapping propagation groups
    // such as {a,b} and {a,c} trigger this reliably under ASan).
    const auto id = pending[index];
    auto *entry = entry_for(id);
    if (entry == nullptr || !visited_entries.insert(entry).second)
      continue;
    if (origin_token != 0) {
      if (entry->last_trigger_origin == origin_token)
        continue;
      entry->last_trigger_origin = origin_token;
    }
    if (!trigger_entry(*entry, now))
      continue;

    triggered.push_back(id);
    if (!expanded_members.insert(id).second)
      continue;
    for (const auto &group : groups_) {
      if (group.mode != CooldownGroupMode::trigger_propagation ||
          std::ranges::find(group.cooldown_ids, id) == group.cooldown_ids.end())
        continue;
      for (const auto &member : group.cooldown_ids)
        if (!expanded_members.contains(member))
          pending.push_back(member);
    }
  }
  return triggered;
}

std::vector<std::string>
CooldownEngine::trigger_with_propagation(const std::string_view cooldown_id,
                                         const TimePoint now,
                                         const std::uint64_t origin_token) {
  const std::array<std::string, 1> seed{std::string(cooldown_id)};
  return trigger_many_with_propagation(seed, now, origin_token);
}

bool CooldownEngine::trigger(const std::string_view cooldown_id,
                             const TimePoint now) {
  // Profiles without groups never propagate; skip the closure bookkeeping.
  if (groups_.empty()) {
    auto *entry = entry_for(cooldown_id);
    return entry != nullptr && trigger_entry(*entry, now);
  }
  return !trigger_with_propagation(cooldown_id, now).empty();
}
bool CooldownEngine::reset(const std::string_view cooldown_id) {
  auto *entry = entry_for(cooldown_id);
  if (entry == nullptr)
    return false;
  entry->available_charges = entry->definition.max_charges;
  entry->next_charge_at.reset();
  entry->recovery_deadlines.clear();
  return true;
}

bool CooldownEngine::refund(const std::string_view cooldown_id,
                            const TimePoint now) {
  auto *entry = entry_for(cooldown_id);
  if (entry == nullptr)
    return false;
  update_entry(*entry, now);
  if (entry->available_charges >= entry->definition.max_charges)
    return false;

  ++entry->available_charges;
  if (entry->definition.recovery == RecoveryMode::parallel &&
      !entry->recovery_deadlines.empty()) {
    // Cancel the latest pending recovery. Deadlines are appended in trigger
    // order, but reduce/restart can disturb that ordering, so pick the
    // maximum explicitly instead of assuming the back element is newest.
    entry->recovery_deadlines.erase(
        std::ranges::max_element(entry->recovery_deadlines));
  }
  if (entry->available_charges >= entry->definition.max_charges) {
    entry->available_charges = entry->definition.max_charges;
    entry->next_charge_at.reset();
    entry->recovery_deadlines.clear();
  } else if (entry->definition.recovery == RecoveryMode::parallel) {
    entry->next_charge_at = earliest_deadline(entry->recovery_deadlines);
  }
  return true;
}

bool CooldownEngine::reduce(const std::string_view cooldown_id,
                            const Milliseconds amount, const bool all_deadlines,
                            const TimePoint now) {
  auto *entry = entry_for(cooldown_id);
  if (entry == nullptr || amount <= Milliseconds::zero())
    return false;
  update_entry(*entry, now);
  if (!entry->next_charge_at)
    return false;

  if (entry->definition.recovery == RecoveryMode::parallel) {
    if (all_deadlines) {
      for (auto &deadline : entry->recovery_deadlines)
        deadline -= amount;
    } else {
      auto earliest = std::ranges::min_element(entry->recovery_deadlines);
      if (earliest == entry->recovery_deadlines.end())
        return false;
      *earliest -= amount;
    }
  } else {
    *entry->next_charge_at -= amount;
  }
  update_entry(*entry, now);
  return true;
}

std::vector<std::string>
CooldownEngine::trigger_group(const std::string_view group_id,
                              const TimePoint now,
                              const std::uint64_t origin_token) {
  const auto found = std::ranges::find(groups_, group_id, &CooldownGroup::id);
  if (found == groups_.end())
    return {};
  if (found->mode == CooldownGroupMode::shared_pool) {
    auto *entry = entry_for(found->cooldown_ids.front());
    if (entry == nullptr ||
        (origin_token != 0 && entry->last_trigger_origin == origin_token))
      return {};
    if (origin_token != 0)
      entry->last_trigger_origin = origin_token;
    return trigger_entry(*entry, now) ? found->cooldown_ids
                                      : std::vector<std::string>{};
  }
  return trigger_many_with_propagation(found->cooldown_ids, now, origin_token);
}

bool CooldownEngine::reset_group(const std::string_view group_id) {
  const auto found = std::ranges::find(groups_, group_id, &CooldownGroup::id);
  if (found == groups_.end())
    return false;
  bool reset_any{};
  std::unordered_set<Entry *> visited;
  for (const auto &id : found->cooldown_ids) {
    auto *entry = entry_for(id);
    if (entry != nullptr && visited.insert(entry).second)
      reset_any = reset(id) || reset_any;
  }
  return reset_any;
}

void CooldownEngine::update(const TimePoint now) {
  for (auto &entry : entries_)
    update_entry(entry, now);
}

std::vector<CooldownState> CooldownEngine::snapshot(const TimePoint now) {
  std::vector<CooldownState> result;
  snapshot(now, result);
  return result;
}

void CooldownEngine::snapshot(const TimePoint now,
                              std::vector<CooldownState> &output) {
  update(now);
  output.resize(order_.size());
  for (std::size_t index = 0; index < order_.size(); ++index) {
    const auto &id = order_[index];
    const auto &entry = entries_[owner_slots_.at(id)];
    auto &state = output[index];
    state.id = id;
    state.available_charges = entry.available_charges;
    state.max_charges = entry.definition.max_charges;
    state.next_charge_at = entry.next_charge_at;
    state.recovery_deadlines = entry.recovery_deadlines;
    state.recharge_duration = entry.definition.duration;
  }
}

std::optional<Milliseconds>
CooldownEngine::remaining(const std::string_view cooldown_id,
                          const TimePoint now) {
  auto *entry = entry_for(cooldown_id);
  if (entry == nullptr)
    return std::nullopt;
  update_entry(*entry, now);
  return cooldown_remaining_ms(entry->next_charge_at, now);
}

void CooldownEngine::update_entry(Entry &entry, const TimePoint now) {
  if (entry.definition.recovery == RecoveryMode::parallel) {
    const auto before = entry.recovery_deadlines.size();
    std::erase_if(entry.recovery_deadlines,
                  [&](const TimePoint deadline) { return deadline <= now; });
    const auto recovered = before - entry.recovery_deadlines.size();
    entry.available_charges = std::min(
        entry.definition.max_charges,
        entry.available_charges + static_cast<std::uint32_t>(recovered));
    entry.next_charge_at = earliest_deadline(entry.recovery_deadlines);
    return;
  }

  if (entry.definition.recovery == RecoveryMode::single) {
    if (entry.next_charge_at && *entry.next_charge_at <= now) {
      entry.available_charges = entry.definition.max_charges;
      entry.next_charge_at.reset();
    }
    return;
  }

  while (entry.next_charge_at && *entry.next_charge_at <= now) {
    ++entry.available_charges;
    if (entry.available_charges >= entry.definition.max_charges) {
      entry.available_charges = entry.definition.max_charges;
      entry.next_charge_at.reset();
    } else {
      *entry.next_charge_at += entry.definition.duration;
    }
  }
}

void CooldownEngine::reconfigure(
    const std::span<const CooldownDefinition> definitions,
    const std::span<const std::string> reset_ids, const TimePoint now,
    const std::span<const CooldownGroup> groups) {
  update(now);
  CooldownEngine next(definitions, groups);
  // Resolve the complete reset set before restoring anything. A later alias
  // must never be able to undo a reset requested through another member.
  std::vector<bool> reset_owners(next.entries_.size());
  for (const auto &id : reset_ids) {
    const auto found = next.owner_slots_.find(id);
    if (found != next.owner_slots_.end())
      reset_owners[found->second] = true;
  }
  for (std::size_t slot = 0; slot < next.entries_.size(); ++slot) {
    if (reset_owners[slot])
      continue;
    auto &replacement = next.entries_[slot];
    const auto previous = std::ranges::find(
        entries_, replacement.definition, &Entry::definition);
    if (previous != entries_.end())
      replacement = *previous;
  }
  *this = std::move(next);
}

} // namespace layerc::core