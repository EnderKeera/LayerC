#pragma once

// Display-server-neutral provider interfaces the session engine consumes when
// a native (non-X11) backend substitutes for the X11 window tracker, monitor
// scan or overlay windows. No backend header may reach this file.

#include "display_types.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::daemon {

// One surface per (profile, panel); the key is stable across refreshes.
class OverlayBackend {
public:
  virtual ~OverlayBackend() = default;
  // Shows or repaints one panel. false when it cannot be shown right now.
  [[nodiscard]] virtual bool show(std::string_view key,
                                  const layerc::platform::OverlayFrame &frame) = 0;
  virtual void hide(std::string_view key) noexcept = 0;
  // Destroys every panel whose key is not listed.
  virtual void retain(std::span<const std::string> keys) noexcept = 0;
  // Empty when panels can be shown; otherwise stable reasons.
  [[nodiscard]] virtual std::vector<std::string> unavailable() const = 0;
};

class NativeDisplaySource {
public:
  virtual ~NativeDisplaySource() = default;
  // Authoritative native window tracking is available.
  [[nodiscard]] virtual bool has_windows() const = 0;
  [[nodiscard]] virtual layerc::platform::WindowScan windows() const = 0;
  [[nodiscard]] virtual bool has_monitors() const = 0;
  [[nodiscard]] virtual layerc::platform::MonitorScan monitors() const = 0;
  // Null when panels keep using X11 overlay windows.
  [[nodiscard]] virtual OverlayBackend *overlay() = 0;
};

} // namespace layerc::daemon
