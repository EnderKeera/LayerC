#pragma once

#include "layerc/core/types.hpp"

#include <algorithm>
#include <chrono>
#include <optional>

namespace layerc::daemon {

// Absolute monotonic deadline for the daemon loop's periodic maintenance
// (isolated-helper watchdogs, live-edit leases). Every wake-up, whatever its
// source, asks whether maintenance is due, and the blocking wait never
// outlasts the time remaining. Continuous input, timer, or snapshot traffic
// therefore cannot postpone an already due check indefinitely.
class MaintenanceSchedule {
public:
  // Arms (or disarms, for an empty interval) the deadline and returns how
  // long the loop may block; empty means no periodic maintenance is needed.
  [[nodiscard]] std::optional<layerc::core::Milliseconds>
  wait_for(const layerc::core::TimePoint now,
           const std::optional<layerc::core::Milliseconds> interval) {
    if (!interval) {
      deadline_.reset();
      return std::nullopt;
    }
    // A shorter interval (e.g. a live-edit session just connected) pulls an
    // armed deadline in; a longer one never pushes a due check out.
    if (!deadline_ || *deadline_ > now + *interval)
      deadline_ = now + *interval;
    if (*deadline_ <= now)
      return layerc::core::Milliseconds{0};
    return std::chrono::ceil<layerc::core::Milliseconds>(*deadline_ - now);
  }

  // True once when the deadline has passed; the next wait_for re-arms it.
  [[nodiscard]] bool take_due(const layerc::core::TimePoint now) noexcept {
    if (!deadline_ || now < *deadline_)
      return false;
    deadline_.reset();
    return true;
  }

private:
  std::optional<layerc::core::TimePoint> deadline_;
};

} // namespace layerc::daemon
