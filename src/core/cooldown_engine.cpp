#include "layerc/core/cooldown_engine.hpp"

#include <algorithm>
#include <stdexcept>

namespace layerc::core {

CooldownEngine::CooldownEngine(
    const std::span<const CooldownDefinition> definitions) {
  entries_.reserve(definitions.size());
  order_.reserve(definitions.size());

  for (const auto &definition : definitions) {
    if (definition.id.empty()) {
      throw std::invalid_argument("cooldown id must not be empty");
    }
    if (definition.duration <= Milliseconds::zero()) {
      throw std::invalid_argument("cooldown duration must be positive");
    }
    if (definition.max_charges == 0) {
      throw std::invalid_argument("cooldown max_charges must be positive");
    }

    Entry entry{
        .definition = definition,
        .available_charges = definition.max_charges,
        .next_charge_at = std::nullopt,
    };
    if (!entries_.emplace(definition.id, entry).second) {
      throw std::invalid_argument("duplicate cooldown id: " + definition.id);
    }
    order_.push_back(definition.id);
  }
}

bool CooldownEngine::trigger(const std::string_view cooldown_id,
                             const TimePoint now) {
  const auto found = entries_.find(std::string(cooldown_id));
  if (found == entries_.end()) {
    return false;
  }

  auto &entry = found->second;
  update_entry(entry, now);

  if (entry.next_charge_at.has_value() && !entry.definition.stack_charges) {
    if (entry.definition.retrigger == RetriggerPolicy::restart) {
      entry.next_charge_at = now + entry.definition.duration;
      return true;
    }
    return false;
  }

  if (entry.available_charges > 0) {
    --entry.available_charges;
    if (!entry.next_charge_at.has_value()) {
      entry.next_charge_at = now + entry.definition.duration;
    }
    return true;
  }

  if (entry.definition.retrigger == RetriggerPolicy::restart) {
    entry.next_charge_at = now + entry.definition.duration;
    return true;
  }

  return false;
}

bool CooldownEngine::reset(const std::string_view cooldown_id) {
  const auto found = entries_.find(std::string(cooldown_id));
  if (found == entries_.end()) {
    return false;
  }

  auto &entry = found->second;
  entry.available_charges = entry.definition.max_charges;
  entry.next_charge_at.reset();
  return true;
}

void CooldownEngine::update(const TimePoint now) {
  for (auto &[id, entry] : entries_) {
    (void)id;
    update_entry(entry, now);
  }
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
    const auto &entry = entries_.at(id);
    auto &state = output[index];
    state.id = id;
    state.available_charges = entry.available_charges;
    state.max_charges = entry.definition.max_charges;
    state.next_charge_at = entry.next_charge_at;
  }
}

std::optional<Milliseconds>
CooldownEngine::remaining(const std::string_view cooldown_id,
                          const TimePoint now) {
  const auto found = entries_.find(std::string(cooldown_id));
  if (found == entries_.end()) {
    return std::nullopt;
  }

  auto &entry = found->second;
  update_entry(entry, now);
  if (!entry.next_charge_at.has_value()) {
    return Milliseconds::zero();
  }
  return std::max(
      Milliseconds::zero(),
      std::chrono::duration_cast<Milliseconds>(*entry.next_charge_at - now));
}

void CooldownEngine::update_entry(Entry &entry, const TimePoint now) {
  while (entry.next_charge_at.has_value() && *entry.next_charge_at <= now) {
    ++entry.available_charges;
    if (entry.available_charges >= entry.definition.max_charges) {
      entry.available_charges = entry.definition.max_charges;
      entry.next_charge_at.reset();
    } else {
      entry.next_charge_at = *entry.next_charge_at + entry.definition.duration;
    }
  }
}

void CooldownEngine::reconfigure(
    const std::span<const CooldownDefinition> definitions,
    const std::span<const std::string> reset_ids, const TimePoint now) {
  update(now);
  std::unordered_map<std::string, Entry> next;
  std::vector<std::string> order;
  next.reserve(definitions.size());
  order.reserve(definitions.size());
  for (const auto &definition : definitions) {
    if (definition.id.empty() || definition.duration <= Milliseconds::zero() ||
        definition.max_charges == 0 || next.contains(definition.id)) {
      throw std::invalid_argument("invalid cooldown reconfiguration");
    }
    Entry entry{.definition = definition,
                .available_charges = definition.max_charges,
                .next_charge_at = std::nullopt};
    const bool reset = std::ranges::find(reset_ids, definition.id) !=
                       reset_ids.end();
    if (!reset) {
      const auto previous = entries_.find(definition.id);
      if (previous != entries_.end()) {
        entry.available_charges = std::min(previous->second.available_charges,
                                           definition.max_charges);
        entry.next_charge_at = previous->second.next_charge_at;
        if (entry.available_charges >= definition.max_charges)
          entry.next_charge_at.reset();
      }
    }
    order.push_back(definition.id);
    next.emplace(definition.id, std::move(entry));
  }
  entries_ = std::move(next);
  order_ = std::move(order);
}

} // namespace layerc::core
