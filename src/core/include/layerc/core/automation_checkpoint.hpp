#pragma once

// Profile checkpoint policy shared with durable persistence.
#include "layerc/core/automation.hpp"

namespace layerc::core {

// Debounce between a persistent-value mutation and its durable checkpoint:
// bursts of timer/input writes collapse into one store write per second.
inline constexpr Milliseconds automation_checkpoint_debounce{1'000};

// Arms the debounce on first mutation; later mutations while armed keep the
// original deadline instead of postponing the checkpoint forever.
void automation_checkpoint_arm(std::optional<TimePoint> &due,
                               TimePoint now) noexcept;

// Whether a durable write should be attempted now (force bypasses the
// deadline for shutdown/flush paths).
[[nodiscard]] bool automation_checkpoint_ready(
    const std::optional<TimePoint> &due, TimePoint now,
    bool force = false) noexcept;

// Report the write outcome with one policy each way: success clears the
// deadline; failure slides it out by one debounce for a retry (never
// re-arms from the old deadline, so a persistently failing store cannot
// pin the checkpoint in the past and busy-loop the daemon).
void automation_checkpoint_report(std::optional<TimePoint> &due,
                                  TimePoint now, bool ok) noexcept;

} // namespace layerc::core
