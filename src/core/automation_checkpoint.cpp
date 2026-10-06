#include "layerc/core/automation_checkpoint.hpp"

namespace layerc::core {

void automation_checkpoint_arm(std::optional<TimePoint> &due,
                               const TimePoint now) noexcept {
  if (!due.has_value()) due = now + automation_checkpoint_debounce;
}

bool automation_checkpoint_ready(
    const std::optional<TimePoint> &due, const TimePoint now,
    const bool force) noexcept {
  return due.has_value() && (force || *due <= now);
}

void automation_checkpoint_report(std::optional<TimePoint> &due,
                                  const TimePoint now,
                                  const bool ok) noexcept {
  due = ok ? std::optional<TimePoint>{}
           : std::optional<TimePoint>{now + automation_checkpoint_debounce};
}

} // namespace layerc::core
