#pragma once

// Display-server-neutral provider interfaces consumed by the session engine.
// No backend header may reach this file.

#include "display_types.hpp"
#include "native_capture.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::daemon {

struct InputDeviceNeeds {
  bool keyboard{};
  bool mouse{};
  bool gamepad{};
};

// One drained batch from a borrowed passive source.
struct PassiveInputPoll {
  std::vector<layerc::core::InputEvent> events;
  bool devices_changed{};
};

// Borrowed passive input source, drained by its owner on the daemon event loop.
class PassiveInputSource {
public:
  virtual ~PassiveInputSource() = default;
  [[nodiscard]] virtual bool input_available() const noexcept = 0;
  [[nodiscard]] virtual bool open_input(std::string &error) = 0;
  virtual void close_input() noexcept = 0;
  [[nodiscard]] virtual bool input_open() const noexcept = 0;
  [[nodiscard]] virtual std::vector<std::string>
  input_unavailable(InputDeviceNeeds needed) const = 0;
  [[nodiscard]] virtual layerc::runtime::RuntimeResourceStats
  input_resources() const noexcept = 0;
  [[nodiscard]] virtual std::string_view input_name() const noexcept = 0;
  [[nodiscard]] virtual PassiveInputPoll poll_input(std::span<const int> ready) {
    (void)ready;
    return {};
  }
  [[nodiscard]] virtual std::string_view input_fault() const noexcept { return {}; }
};

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

class DisplaySource {
public:
  virtual ~DisplaySource() = default;
  // Window tracking is available.
  [[nodiscard]] virtual bool has_windows() const = 0;
  // Compositor reports supersede XWayland-only scans; foreign toplevels
  // retain the existing fallback because they cannot supply PIDs.
  [[nodiscard]] virtual bool prefer_windows() const { return false; }
  [[nodiscard]] virtual CaptureFactory *capture() { return nullptr; }
  [[nodiscard]] virtual PassiveInputSource *input() { return nullptr; }
  [[nodiscard]] virtual layerc::platform::WindowScan windows() const = 0;
  [[nodiscard]] virtual bool has_monitors() const = 0;
  [[nodiscard]] virtual layerc::platform::MonitorScan monitors() const = 0;
  // Null when this source cannot show panels.
  [[nodiscard]] virtual OverlayBackend *overlay() = 0;
};

} // namespace layerc::daemon
