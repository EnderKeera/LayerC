#pragma once

#include "layerc/core/panel_presentation.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <span>

namespace layerc::daemon {
struct OverlayCadence {
  bool continuous{};
  std::optional<layerc::core::Milliseconds> delay;
};

// Static items never poll. Countdown rows use 16 ms; flash-only frames wake at
// their next core-computed edge. Hidden panels contribute no demand.
[[nodiscard]] inline OverlayCadence overlay_cadence(
    const std::span<const layerc::core::PanelPresentation> frames,
    bool cooldown_running, const layerc::core::TimePoint now) {
  using namespace layerc::core;
  std::optional<TimePoint> next;
  for (const auto &frame : frames) {
    if (!frame.visible) continue;
    next = earliest_deadline(next, frame.next_wakeup);
    for (const auto &item : frame.items) {
      if (item.unavailable) continue;
      cooldown_running = cooldown_running ||
          (item.kind == OverlayItemKind::cooldown && !item.ready) ||
          (item.kind == OverlayItemKind::timer && item.active);
    }
  }
  if (cooldown_running) return {true, Milliseconds{16}};
  if (!next) return {};
  return {false, std::max(Milliseconds{1}, std::chrono::ceil<Milliseconds>(*next - now))};
}
} // namespace layerc::daemon
