#pragma once

#include "layerc/platform/xinput_controls.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace layerc::editor {

// Capture is passive: a dialog records only while it owns native focus and
// the foreground. Foreground ownership is read through one seam so tests can
// exercise both sides of the gate without changing the user's desktop.
using ForegroundQuery = HWND (*)() noexcept;
[[nodiscard]] inline HWND system_foreground_window() noexcept { return GetForegroundWindow(); }
inline ForegroundQuery foreground_window_query = &system_foreground_window;
[[nodiscard]] inline bool owns_native_focus(const HWND window) noexcept {
  return window != nullptr && ::GetFocus() == window &&
         foreground_window_query() == window;
}

// Reset on loss of focus. A controller that connects or reconnects supplies a
// baseline rather than a press, so an input held outside capture is not bound.
class FocusedControllerEdges final {
public:
  void reset() noexcept { baselines_ = {}; }

  [[nodiscard]] std::uint32_t
  sample(const DWORD index, const std::optional<XINPUT_GAMEPAD> &state) noexcept {
    if (index >= baselines_.size()) return 0;
    auto &previous = baselines_[index];
    if (!state) {
      previous.reset();
      return 0;
    }
    const auto current = platform::xinput_digital_controls(*state) &
                         platform::xinput_bindable_controls;
    const auto pressed = previous ? current & ~*previous : 0U;
    previous = current;
    return pressed;
  }

private:
  std::array<std::optional<std::uint32_t>, XUSER_MAX_COUNT> baselines_{};
};

} // namespace layerc::editor
