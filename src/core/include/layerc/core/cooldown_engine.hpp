#pragma once

#include "layerc/core/cooldown_groups.hpp"
#include "layerc/core/string_hash.hpp"
#include "layerc/core/types.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace layerc::core {

// "3.0s": whole tenths, rounded up so a running clock never shows 0.0s.
[[nodiscard]] std::string format_tenths(Milliseconds remaining);
// "READY" or the remaining tenths, plus "  [n]" for multi-charge cooldowns.
[[nodiscard]] std::string cooldown_status_value(const CooldownState &state,
                                                TimePoint now);
// The cooldown's name, two spaces, then cooldown_status_value.
[[nodiscard]] std::string cooldown_status_text(
    const CooldownDefinition &definition, const CooldownState &state,
    TimePoint now);
[[nodiscard]] Milliseconds cooldown_remaining_ms(
    const std::optional<TimePoint> &next_charge_at, TimePoint now) noexcept;
// Native bars show the remaining fraction of the effective owner duration;
// ready is 1.0.
[[nodiscard]] double cooldown_progress(const CooldownState &state,
                                       TimePoint now) noexcept;
// Engine snapshots follow the definition order, so a definition's state is
// normally at the definition's own position. Falls back to an id search if a
// caller's states ever drift from its definitions; nullptr when absent.
[[nodiscard]] const CooldownState *
cooldown_state_at(std::span<const CooldownState> states, std::size_t index,
                  std::string_view cooldown_id) noexcept;

class CooldownEngine {
public:
  explicit CooldownEngine(std::span<const CooldownDefinition> definitions,
                          std::span<const CooldownGroup> groups = {});

  // False when arming any active window at `now` would leave the monotonic
  // clock range; every trigger API then refuses without mutating.
  [[nodiscard]] bool can_arm_active(TimePoint now) const noexcept;
  [[nodiscard]] bool trigger(std::string_view cooldown_id, TimePoint now);
  [[nodiscard]] std::vector<std::string>
  trigger_with_propagation(std::string_view cooldown_id, TimePoint now,
                           std::uint64_t origin_token = 0);
  [[nodiscard]] bool reset(std::string_view cooldown_id);
  [[nodiscard]] bool refund(std::string_view cooldown_id, TimePoint now);
  [[nodiscard]] bool reduce(std::string_view cooldown_id, Milliseconds amount,
                            bool all_deadlines, TimePoint now);
  [[nodiscard]] std::vector<std::string>
  trigger_group(std::string_view group_id, TimePoint now,
                std::uint64_t origin_token = 0);
  [[nodiscard]] bool reset_group(std::string_view group_id);
  void update(TimePoint now);

  [[nodiscard]] std::vector<CooldownState> snapshot(TimePoint now);
  void snapshot(TimePoint now, std::vector<CooldownState> &output);
  [[nodiscard]] std::optional<Milliseconds>
  remaining(std::string_view cooldown_id, TimePoint now);
  // The canonical effective runtime definition, shared by aliases. Labels and
  // dormant member settings do not participate in owner reconciliation.
  [[nodiscard]] const CooldownOwnerDefinition *
  owner_definition(std::string_view cooldown_id) const;
  [[nodiscard]] const CooldownGroupTopology &group_topology() const noexcept {
    return groups_;
  }
  // Preserve only unchanged owners; any reset alias resets its entire owner.
  void reconfigure(std::span<const CooldownDefinition> definitions,
                   std::span<const std::string> reset_ids, TimePoint now,
                   std::span<const CooldownGroup> groups = {});

private:
  struct Entry {
    CooldownOwnerDefinition definition;
    std::uint32_t available_charges{};
    std::optional<TimePoint> next_charge_at;
    std::vector<TimePoint> recovery_deadlines;
    std::uint64_t last_trigger_origin{};
  };

  // Activity belongs to a public cooldown ID, never to its shared owner.
  struct Activity {
    Milliseconds duration{};
    std::optional<TimePoint> until;
    std::size_t owner{};
  };

  void update_entry(Entry &entry, TimePoint now);
  void arm(std::string_view cooldown_id, TimePoint now);
  void clear_owner_activity(std::size_t owner);
  [[nodiscard]] bool trigger_entry(Entry &entry, TimePoint now);
  // One root origin triggers an owner at most once; false when already done.
  [[nodiscard]] static bool claim_origin(Entry &entry, std::uint64_t origin_token);
  [[nodiscard]] std::vector<std::string>
  trigger_many_with_propagation(std::span<const std::string> cooldown_ids,
                                TimePoint now, std::uint64_t origin_token);
  [[nodiscard]] Entry *entry_for(std::string_view cooldown_id);
  [[nodiscard]] const Entry *entry_for(std::string_view cooldown_id) const;

  std::vector<std::string> order_;
  // Exactly one definition and state per effective owner, no dormant entries.
  std::vector<Entry> entries_;
  std::unordered_map<std::string, std::size_t, TransparentStringHash,
                     std::equal_to<>>
      owner_slots_;
  // Parallel to order_; public_slots_ maps an ID to its index there.
  std::vector<Activity> activity_;
  std::unordered_map<std::string, std::size_t, TransparentStringHash,
                     std::equal_to<>>
      public_slots_;
  Milliseconds max_active_duration_{};
  CooldownGroupTopology groups_;
};

} // namespace layerc::core
