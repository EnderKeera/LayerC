#pragma once

#include "display_selection.hpp"
#include "event_loop.hpp"
#include "evdev_input.hpp"

#include "layerc/runtime/runtime_services.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace layerc::daemon {

// Stable status tokens for evdev reasons: the text before the first ':' (the
// device path and errno text stay in diagnostics), deduplicated in order.
[[nodiscard]] std::vector<std::string>
evdev_reason_tokens(std::span<const std::string> reasons);

// The daemon's passive evdev input source on the one EventLoop. Devices open
// only between open() and close(), which the input service drives from
// demand; every descriptor is registered while open and unregistered before
// it is closed, so the loop must outlive an open route. Read-only: the source
// never grabs or writes a device.
class EvdevInputRoute {
public:
  EvdevInputRoute() = default;
  ~EvdevInputRoute();
  EvdevInputRoute(const EvdevInputRoute &) = delete;
  EvdevInputRoute &operator=(const EvdevInputRoute &) = delete;

  void configure(const InputOptions &options);
  void attach(layerc::platform::EventLoop &loop) noexcept { loop_ = &loop; }

  [[nodiscard]] bool enabled() const noexcept {
    return options_.selection == InputSelection::evdev;
  }
  [[nodiscard]] bool attached() const noexcept { return source_.has_value(); }

  // False with a reason when the source cannot be created or every device is
  // denied (the caller may fall back to XI2). An invalid compositor keymap
  // falls back to the XKB defaults and is reported, not fatal.
  [[nodiscard]] bool open(std::string &error);
  void close() noexcept;
  // Drains ready device descriptors and hotplug notices, then replaces the
  // loop registrations when the device set changed.
  [[nodiscard]] layerc::platform::EvdevPoll poll(std::span<const int> ready);

  // Applied now when open and kept for later opens; nullopt is the default.
  void set_keymap(const std::optional<std::string> &text);
  [[nodiscard]] std::vector<std::string>
  unavailable(layerc::platform::EvdevInputCapabilities needed) const;
  // The last open or keymap failure; empty after a clean open.
  [[nodiscard]] const std::string &fault() const noexcept { return fault_; }
  [[nodiscard]] std::span<const int> registered() const noexcept { return registered_; }
  [[nodiscard]] layerc::runtime::RuntimeResourceStats resources() const noexcept;

private:
  void sync_registrations() noexcept;

  InputOptions options_;
  layerc::platform::EventLoop *loop_{};
  std::optional<layerc::platform::EvdevInputSource> source_;
  std::vector<int> registered_;
  std::optional<std::string> keymap_;
  std::string fault_;
};

} // namespace layerc::daemon
