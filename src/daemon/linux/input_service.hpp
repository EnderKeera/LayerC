#pragma once

#include "x11_display.hpp"
#include "x11_input.hpp"
#include "x11_window.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/input_matcher.hpp"
#include "layerc/core/pixel_engine.hpp"
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
struct InputSession {
  std::filesystem::path source_path;
  layerc::core::GameProfile profile;
  std::uint64_t window{};
  bool global{};
  bool active{};
  layerc::core::InputMatcher matcher;
  layerc::core::CooldownEngine engine;
  std::optional<layerc::core::VisualProfile> visual_profile;
  std::unique_ptr<layerc::core::VisualTriggerEngine> visual_triggers;
  std::unique_ptr<layerc::core::AutomationRuntime> automation;
  std::optional<layerc::core::TimePoint> automation_state_due;
  std::uint64_t automation_origin_sequence{1};
  std::vector<layerc::core::CooldownState> cooldown_snapshot;

  InputSession(
      layerc::platform::StoredProfile source, std::uint64_t window, bool global,
      std::optional<layerc::core::VisualProfile> visual_profile = {},
      std::optional<layerc::core::AutomationProfile> automation_profile = {},
      layerc::core::AutomationPersistentValues automation_state = {});

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
  [[nodiscard]] layerc::core::AutomationEvent
  automation_event(layerc::core::AutomationEventKind kind,
                   std::string source_id, layerc::core::TimePoint now,
                   std::optional<std::uint64_t> origin = std::nullopt);
  void process_automation_event(const layerc::core::AutomationEvent &event);
  void trigger_requests(
      std::span<const layerc::core::VisualTriggerRequest> requests,
      layerc::core::TimePoint now, std::vector<std::string> &triggered);
  void poll_cooldown_transitions(layerc::core::TimePoint now, bool emit);
  void synchronize_automation_state(layerc::core::TimePoint now);
  void checkpoint_automation_state(layerc::core::TimePoint now, bool force);
};

struct X11InputDispatch {
  std::vector<std::string> triggered;
  std::size_t events{};
  bool windows_changed{};
  bool devices_changed{};
};

// The daemon's Capability::input service on X11: passive XI2 raw input
// feeding profile sessions. The XI2 device source exists only while the
// kernel demands input, so an idle daemon holds no input resources.
class X11InputService final : public layerc::runtime::IRuntimeService {
public:
  // The display outlives the service; the shared connection owner keeps it.
  void attach(layerc::platform::X11Display &display) noexcept;

  // Rebuilds sessions from the profiles that resolve in the scan. A global
  // profile follows the active window; an unmatched profile gets no session.
  void refresh(std::span<const layerc::platform::StoredProfile> profiles,
               const layerc::platform::X11WindowScan &scan);

  // Final shutdown runs session-stopping rules; capability release does not.
  void stop_sessions(layerc::core::TimePoint now);

  // Drains the X connection and feeds every normalized event to the
  // sessions. Unknown events are discarded so the socket cannot busy-poll.
  [[nodiscard]] X11InputDispatch dispatch();

  [[nodiscard]] std::size_t session_count() const noexcept {
    return sessions_.size();
  }
  // Sessions are the daemon's whole view of what is running: the overlay
  // service paints from the same list, so profiles stay in one place.
  [[nodiscard]] std::span<InputSession> sessions() noexcept { return sessions_; }
  [[nodiscard]] std::span<const InputSession> sessions() const noexcept {
    return sessions_;
  }
  [[nodiscard]] bool attached() const noexcept {
    return source_.has_value();
  }
  [[nodiscard]] std::vector<layerc::runtime::SessionCapabilityRequest>
  session_requests() const;
  [[nodiscard]] std::vector<std::string> unavailable() const;

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

  [[nodiscard]] layerc::runtime::Capability capability() const noexcept override {
    return layerc::runtime::Capability::input;
  }
  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

private:
  layerc::platform::X11Display *display_{};
  std::optional<layerc::platform::X11InputSource> source_;
  std::vector<InputSession> sessions_;
  std::vector<std::string> automation_diagnostics_;
  std::vector<std::string> visual_diagnostics_;
};

class X11AutomationService final : public layerc::runtime::IRuntimeService {
public:
  explicit X11AutomationService(X11InputService &input) noexcept
      : input_(input) {}

  [[nodiscard]] layerc::runtime::Capability capability() const noexcept override {
    return layerc::runtime::Capability::automation;
  }
  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

private:
  X11InputService &input_;
};

} // namespace layerc::daemon
