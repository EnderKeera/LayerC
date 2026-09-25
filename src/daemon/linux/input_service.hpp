#pragma once

#include "x11_adapters.hpp"
#include "x11_display.hpp"
#include "x11_input.hpp"
#include "x11_window.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/input_matcher.hpp"
#include "layerc/core/pixel_engine.hpp"
#include "layerc/core/profile_session.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace layerc::daemon {

// One matched profile: its bindings drive a matcher and every match drives
// the profile's cooldown engine. Bindings the X11 adapter cannot produce
// (controller or HID tokens) stay in the matcher and simply never fire.
struct InputSession : layerc::core::AutomationSession {
  std::filesystem::path source_path;
  layerc::core::GameProfile profile;
  std::uint64_t window{};
  bool global{};
  bool active{};
  layerc::core::InputMatcher matcher;
  std::optional<layerc::core::VisualProfile> visual_profile;
  std::optional<layerc::core::TimePoint> automation_state_due;
  std::vector<std::string> automation_faults;

  InputSession(
      layerc::platform::StoredProfile source, std::uint64_t window, bool global,
      std::optional<layerc::core::VisualProfile> visual_profile = {},
      std::optional<layerc::core::AutomationProfile> automation_profile = {},
      layerc::core::AutomationPersistentValues automation_state = {},
      bool invalid_visual_sidecar = false);

  // Cooldown ids the event actually triggered (a chord that is not yet
  // complete, or a cooldown still running, yields nothing).
  [[nodiscard]] std::vector<std::string>
  feed(const layerc::core::InputEvent &event, layerc::core::TimePoint now);
  [[nodiscard]] std::vector<std::string> feed_measurements(
      std::span<const layerc::core::ObservationMeasurement> measurements,
      layerc::core::TimePoint now);
  void poll_automation(layerc::core::TimePoint now);
  [[nodiscard]] std::optional<layerc::core::TimePoint>
  next_automation_deadline(layerc::core::TimePoint now);
  void stop_automation(layerc::core::TimePoint now);
  // Capability quiesce must checkpoint persistent automation values without
  // destroying the session: matched application sessions live above the
  // input adapter, so input/XI2 teardown releases only the device source.
  void checkpoint_automation_now(layerc::core::TimePoint now);

private:
  void checkpoint_automation_state(layerc::core::TimePoint now, bool force);
};

// Owns the profile sessions for the whole daemon, mirroring the Windows
// SessionRegistry: input feeds them, automation checkpoints them, and the
// overlay and visual services paint and sample from the same list.
class X11SessionRegistry {
public:
  // Rebuilds sessions from the profiles that resolve in the scan. A global
  // profile follows the active window; an unmatched profile gets no session.
  void refresh(std::span<const layerc::platform::StoredProfile> profiles,
               const layerc::platform::X11WindowScan &scan);

  // Final shutdown runs session-stopping rules; capability release does not.
  void stop_sessions(layerc::core::TimePoint now);

  [[nodiscard]] std::size_t session_count() const noexcept {
    return sessions_.size();
  }
  [[nodiscard]] std::span<InputSession> sessions() noexcept { return sessions_; }
  [[nodiscard]] std::span<const InputSession> sessions() const noexcept {
    return sessions_;
  }
  [[nodiscard]] std::vector<layerc::runtime::SessionCapabilityRequest>
  session_requests() const;

  [[nodiscard]] std::size_t automation_count() const noexcept;
  void poll_automation(layerc::core::TimePoint now);
  [[nodiscard]] std::optional<layerc::core::TimePoint>
  next_automation_deadline(layerc::core::TimePoint now);
  [[nodiscard]] std::span<const std::string> automation_diagnostics() const {
    return automation_diagnostics_;
  }
  [[nodiscard]] std::span<const std::string> visual_diagnostics() const {
    return visual_diagnostics_;
  }

private:
  std::vector<InputSession> sessions_;
  std::vector<std::string> automation_diagnostics_;
  std::vector<std::string> visual_diagnostics_;
};

// The daemon's Capability::input service on X11: passive XI2 raw input
// feeding the registry's sessions. The XI2 device source exists only while
// the kernel demands input, so an idle daemon holds no input resources.
class X11InputService final : public layerc::runtime::IRuntimeService {
public:
  explicit X11InputService(X11SessionRegistry &registry) noexcept
      : registry_(registry) {}

  // The display outlives the service; the shared connection owner keeps it.
  void attach(layerc::platform::X11Display &display) noexcept;

  // Drains the X connection and feeds every normalized event to the
  // sessions. Unknown events are discarded so the socket cannot busy-poll.
  [[nodiscard]] X11Dispatch dispatch();

  [[nodiscard]] bool attached() const noexcept {
    return source_.has_value();
  }
  [[nodiscard]] std::vector<std::string> unavailable() const;

  [[nodiscard]] layerc::runtime::Capability capability() const noexcept override {
    return layerc::runtime::Capability::input;
  }
  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

private:
  X11SessionRegistry &registry_;
  layerc::platform::X11Display *display_{};
  std::optional<layerc::platform::X11InputSource> source_;
};

class X11AutomationService final : public layerc::runtime::IRuntimeService {
public:
  explicit X11AutomationService(X11SessionRegistry &registry) noexcept
      : registry_(registry) {}

  [[nodiscard]] layerc::runtime::Capability capability() const noexcept override {
    return layerc::runtime::Capability::automation;
  }
  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

private:
  X11SessionRegistry &registry_;
};

} // namespace layerc::daemon
