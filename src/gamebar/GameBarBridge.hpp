#pragma once

#include "layerc/core/companion.hpp"
#include "layerc/core/types.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace layerc::gamebar {

// Bridge that serializes bounded, immutable snapshot messages for the
// AppService-based Game Bar companion.
class GameBarBridge {
public:
  GameBarBridge() = default;
  ~GameBarBridge() = default;

  // Formats an immutable snapshot of foreground panel states from current
  // session cooldown and panel definitions.
  [[nodiscard]] static layerc::core::CompanionSessionSnapshot
  create_session_snapshot(
      std::string_view profile_id, std::string_view session_key,
      std::span<const layerc::core::Panel> panels,
      std::span<const layerc::core::CooldownDefinition> definitions,
      std::span<const layerc::core::CooldownState> states,
      const std::optional<layerc::core::CompanionProfile> &companion =
          std::nullopt,
      layerc::core::TimePoint now = layerc::core::MonotonicClock::now());

  void update_snapshot(layerc::core::CompanionSessionSnapshot snapshot);
  void clear_snapshot() noexcept;
  [[nodiscard]] std::optional<layerc::core::CompanionSessionSnapshot>
  latest_snapshot(layerc::core::TimePoint now =
                      layerc::core::MonotonicClock::now()) const;
  [[nodiscard]] std::optional<std::string>
  latest_snapshot_json(layerc::core::TimePoint now =
                           layerc::core::MonotonicClock::now()) const;

  // Stale data cutoff is 2.0 seconds per V2.4 specification
  static constexpr std::chrono::milliseconds stale_timeout{2000};

private:
  mutable std::mutex mutex_;
  std::optional<layerc::core::CompanionSessionSnapshot> latest_snapshot_;
  layerc::core::TimePoint last_update_time_{};
};

} // namespace layerc::gamebar
