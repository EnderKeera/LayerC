#pragma once

#include "layerc/capture/capture.hpp"
#include "layerc/runtime/display_diagnostics.hpp"

#include <string>
#include <vector>

namespace layerc::daemon {

// The published state of one portal capture session. A portal grant selects a
// stream; it never verifies that the stream is the tracked window, so an
// available portal stream is degraded with the binding that produced it and is
// never reported active.
[[nodiscard]] inline layerc::runtime::DisplayProviderStatus portal_capture_status(
    const bool started, const layerc::capture::CapturePollResult &result) {
  using layerc::runtime::DisplayProviderState;
  using layerc::capture::CaptureTargetBinding;
  if (!started)
    return {"portal_screencast", DisplayProviderState::inactive,
            {"not_demanded"}};
  if (result.availability != layerc::core::CaptureAvailability::available)
    return {"portal_screencast", DisplayProviderState::unavailable,
            {result.diagnostic}};
  if (result.binding == CaptureTargetBinding::monitor_crop)
    return {"portal_screencast", DisplayProviderState::degraded,
            {"monitor_crop"}};
  return {"portal_screencast", DisplayProviderState::degraded,
          {"user_selected_window"}};
}

} // namespace layerc::daemon
