#include "layerc/core/cooldown_groups.hpp"

#include <algorithm>
#include <set>

namespace layerc::core {
namespace {

[[nodiscard]] bool contains(const CooldownGroup &group,
                            const std::string_view cooldown_id) {
  return std::ranges::find(group.cooldown_ids, cooldown_id) !=
         group.cooldown_ids.end();
}

// Appends each id not yet in `seen`, preserving first-seen order.
void append_unique(std::vector<std::string> &result,
                   std::set<std::string, std::less<>> &seen,
                   const std::span<const std::string> ids) {
  for (const auto &id : ids)
    if (seen.insert(id).second) result.push_back(id);
}

} // namespace

CooldownOwnerDefinition standalone_owner(const CooldownDefinition &definition) {
  return {.kind = CooldownOwnerKind::cooldown,
          .id = definition.id,
          .duration = definition.duration,
          .max_charges = definition.max_charges,
          .retrigger = definition.retrigger,
          .recovery = definition.recovery};
}

CooldownOwnerDefinition pool_owner(const CooldownGroup &pool) {
  return {.kind = CooldownOwnerKind::shared_pool,
          .id = pool.id,
          .duration = pool.duration,
          .max_charges = pool.max_charges,
          .retrigger = pool.retrigger,
          .recovery = pool.recovery};
}

CooldownGroupTopology::CooldownGroupTopology(
    const std::span<const CooldownGroup> groups)
    : groups_(groups.begin(), groups.end()) {}

const CooldownGroup *
CooldownGroupTopology::find(const std::string_view group_id) const {
  const auto found = std::ranges::find(groups_, group_id, &CooldownGroup::id);
  return found == groups_.end() ? nullptr : &*found;
}

const CooldownGroup *
CooldownGroupTopology::pool_of(const std::string_view cooldown_id) const {
  const auto found = std::ranges::find_if(groups_, [&](const auto &group) {
    return group.mode == CooldownGroupMode::shared_pool &&
           contains(group, cooldown_id);
  });
  return found == groups_.end() ? nullptr : &*found;
}

std::optional<CooldownOwnerDefinition>
CooldownGroupTopology::owner(const std::span<const CooldownDefinition> definitions,
                             const std::string_view cooldown_id) const {
  const auto definition =
      std::ranges::find(definitions, cooldown_id, &CooldownDefinition::id);
  if (definition == definitions.end()) return std::nullopt;
  if (const auto *pool = pool_of(cooldown_id)) return pool_owner(*pool);
  return standalone_owner(*definition);
}

std::vector<std::string>
CooldownGroupTopology::propagation_step(const std::string_view cooldown_id) const {
  std::vector<std::string> result;
  for (const auto &group : groups_)
    if (group.mode == CooldownGroupMode::trigger_propagation &&
        contains(group, cooldown_id))
      result.insert(result.end(), group.cooldown_ids.begin(),
                    group.cooldown_ids.end());
  return result;
}

std::vector<std::string> CooldownGroupTopology::trigger_closure(
    const std::span<const std::string> seeds) const {
  std::vector<std::string> result;
  std::set<std::string, std::less<>> seen;
  append_unique(result, seen, seeds);
  for (std::size_t index{}; index < result.size(); ++index) {
    const auto step = propagation_step(result[index]);
    append_unique(result, seen, step);
  }
  return result;
}

std::vector<std::string> CooldownGroupTopology::group_trigger_members(
    const std::string_view group_id) const {
  const auto *group = find(group_id);
  if (group == nullptr) return {};
  if (group->mode == CooldownGroupMode::shared_pool) return group->cooldown_ids;
  return trigger_closure(group->cooldown_ids);
}

std::vector<std::string>
CooldownGroupTopology::reset_members(const std::string_view cooldown_id) const {
  if (const auto *pool = pool_of(cooldown_id)) return pool->cooldown_ids;
  return {std::string(cooldown_id)};
}

std::vector<std::string> CooldownGroupTopology::group_reset_members(
    const std::string_view group_id) const {
  const auto *group = find(group_id);
  if (group == nullptr) return {};
  std::vector<std::string> result;
  std::set<std::string, std::less<>> seen;
  for (const auto &id : group->cooldown_ids)
    append_unique(result, seen, reset_members(id));
  return result;
}

} // namespace layerc::core
