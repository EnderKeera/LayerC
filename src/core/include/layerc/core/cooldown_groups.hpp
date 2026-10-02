#pragma once

// Single owner of cooldown-group semantics. CooldownEngine applies these sets
// to real charge state, the automation runtime derives intra-batch events from
// them, and profile diffs resolve owners through them, so the three can never
// disagree about which cooldowns a group touches.

#include "layerc/core/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::core {

// Runtime identities have separate domains. Persisted cooldown and group IDs
// remain opaque: no synthetic string can collide with a public ID.
enum class CooldownOwnerKind : std::uint8_t { cooldown, shared_pool };

struct CooldownOwnerDefinition {
  CooldownOwnerKind kind{CooldownOwnerKind::cooldown};
  std::string id;
  Milliseconds duration{};
  std::uint32_t max_charges{1};
  RetriggerPolicy retrigger{RetriggerPolicy::ignore};
  RecoveryMode recovery{RecoveryMode::single};

  bool operator==(const CooldownOwnerDefinition &) const = default;
};

[[nodiscard]] CooldownOwnerDefinition
standalone_owner(const CooldownDefinition &definition);
[[nodiscard]] CooldownOwnerDefinition pool_owner(const CooldownGroup &pool);

class CooldownGroupTopology {
public:
  CooldownGroupTopology() = default;
  explicit CooldownGroupTopology(std::span<const CooldownGroup> groups);

  [[nodiscard]] std::span<const CooldownGroup> groups() const noexcept {
    return groups_;
  }
  [[nodiscard]] bool empty() const noexcept { return groups_.empty(); }
  [[nodiscard]] const CooldownGroup *find(std::string_view group_id) const;
  // The shared pool a cooldown belongs to, if any.
  [[nodiscard]] const CooldownGroup *pool_of(std::string_view cooldown_id) const;

  // The effective owner of a public cooldown: its shared pool when it has
  // one, otherwise the definition itself. Nullopt for an unknown id.
  [[nodiscard]] std::optional<CooldownOwnerDefinition>
  owner(std::span<const CooldownDefinition> definitions,
        std::string_view cooldown_id) const;

  // One propagation step: every member of every trigger-propagation group
  // containing `cooldown_id`, in group order.
  [[nodiscard]] std::vector<std::string>
  propagation_step(std::string_view cooldown_id) const;
  // Seeds first, then the breadth-first propagation closure, each id once.
  [[nodiscard]] std::vector<std::string>
  trigger_closure(std::span<const std::string> seeds) const;
  // trigger_group: a shared pool consumes one pool charge for its members;
  // any other group triggers the propagation closure of its members.
  [[nodiscard]] std::vector<std::string>
  group_trigger_members(std::string_view group_id) const;
  // reset_cooldown restores the cooldown's owner: its whole shared pool.
  [[nodiscard]] std::vector<std::string>
  reset_members(std::string_view cooldown_id) const;
  // reset_group restores the owner of every member, each id once.
  [[nodiscard]] std::vector<std::string>
  group_reset_members(std::string_view group_id) const;

private:
  std::vector<CooldownGroup> groups_;
};

} // namespace layerc::core
