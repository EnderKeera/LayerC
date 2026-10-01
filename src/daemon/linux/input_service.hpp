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
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace layerc::daemon {

// A stopped runtime's last value copy stays owned until its durable write
// succeeds; the core runtime is destroyed after session_stopping.
struct RetiredAutomationState {
  layerc::core::AutomationProfile profile;
  layerc::core::AutomationPersistentValues values;
  std::optional<layerc::core::TimePoint> due;
  std::optional<std::string> checkpoint_error;
};

// References into one loaded registry revision. Visual assets can be prepared
// before a profile has a matching window.
struct VisualConfiguration {
  const std::filesystem::path &source_path;
  const std::string &profile_id;
  const layerc::core::VisualProfile &profile;
};

// One matched profile owns cooldown, automation, and visual state. Direct
// bindings are optional; bindings the X11 adapter cannot produce (controller
// or HID tokens) stay in the matcher and simply never fire.
struct InputSession : layerc::core::AutomationSession {
  std::filesystem::path source_path;
  std::uint64_t window{};
  bool global{};
  bool active{};
  layerc::core::InputMatcher matcher;
  std::optional<layerc::core::VisualProfile> visual_profile;
  bool invalid_automation_sidecar{};
  std::optional<layerc::core::TimePoint> automation_state_due;
  struct AutomationHealth {
    std::optional<std::string> checkpoint_error;
    std::optional<std::string> state_load_error;
    std::optional<std::string> runtime_error;
    // At most one current fault per automation in the validated profile.
    std::map<std::string, std::string, std::less<>> runtime_faults;
  } automation_health;
  bool deferred_automation{};
  // "<device>:<code>:press|release" of the last event a binding consumed,
  // as the Windows daemon reports it.
  std::string last_input_match;

  InputSession(
      layerc::platform::StoredProfile source, std::uint64_t window, bool global,
      std::optional<layerc::core::VisualProfile> visual_profile = {},
      std::optional<layerc::core::AutomationProfile> automation_profile = {},
      layerc::core::AutomationPersistentValues automation_state = {},
      bool invalid_visual_sidecar = false,
      bool invalid_automation_sidecar = false,
      std::optional<std::string> automation_state_load_error = {});

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
  [[nodiscard]] std::optional<RetiredAutomationState>
  stop_automation(layerc::core::TimePoint now);
  void start_deferred_automation(
      const layerc::core::AutomationProfile &profile,
      layerc::core::AutomationPersistentValues values,
      layerc::core::TimePoint now,
      std::optional<std::string> state_load_error = {});
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
  // Load one configuration revision, including optional module sidecars and
  // state for explicit clean-runtime reloads. Retained window matches reuse
  // configuration; fresh automation admissions reload transactional state.
  void configure(std::span<const layerc::platform::StoredProfile> profiles);
  // Rebuilds sessions from configured profiles that resolve in the scan. A
  // global profile follows the active window; an unmatched profile has none.
  void refresh(const layerc::platform::X11WindowScan &scan);

  // Final shutdown runs session-stopping rules; capability release does not.
  void stop_sessions(layerc::core::TimePoint now);

  [[nodiscard]] std::size_t session_count() const noexcept {
    return sessions_.size();
  }
  [[nodiscard]] std::span<InputSession> sessions() noexcept { return sessions_; }
  [[nodiscard]] std::span<const InputSession> sessions() const noexcept {
    return sessions_;
  }
  [[nodiscard]] std::span<const VisualConfiguration>
  visual_configurations() const noexcept { return visual_configurations_; }
  [[nodiscard]] std::vector<layerc::runtime::SessionCapabilityRequest>
  session_requests() const;

  [[nodiscard]] std::size_t automation_count() const noexcept;
  [[nodiscard]] std::size_t pending_checkpoint_count() const noexcept {
    return pending_checkpoints_.size();
  }
  // True when a pending checkpoint completes and capability demand changes.
  [[nodiscard]] bool poll_automation(layerc::core::TimePoint now);
  [[nodiscard]] std::optional<layerc::core::TimePoint>
  next_automation_deadline(layerc::core::TimePoint now);
  [[nodiscard]] std::vector<std::string> automation_diagnostics() const;
  [[nodiscard]] std::span<const std::string> visual_diagnostics() const {
    return visual_diagnostics_;
  }

private:
  struct ConfiguredProfile {
    layerc::platform::StoredProfile source;
    std::optional<layerc::core::VisualProfile> visual_profile;
    std::optional<layerc::core::AutomationProfile> automation_profile;
    layerc::core::AutomationPersistentValues automation_state;
    std::string visual_diagnostic;
    std::string automation_sidecar_diagnostic;
    std::optional<std::string> automation_state_diagnostic;
    bool invalid_visual_sidecar{};
    bool invalid_automation_sidecar{};
  };

  [[nodiscard]] std::optional<RetiredAutomationState>
  retire_session(InputSession &session, layerc::core::TimePoint now);
  [[nodiscard]] bool retry_pending_checkpoints(layerc::core::TimePoint now,
                                               bool force = false);

  std::vector<ConfiguredProfile> configured_;
  std::vector<VisualConfiguration> visual_configurations_;
  std::vector<InputSession> sessions_;
  std::map<std::filesystem::path, RetiredAutomationState> pending_checkpoints_;
  std::vector<std::string> matched_automation_diagnostics_;
  std::vector<std::string> visual_diagnostics_;
  bool configuration_changed_{};
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

  [[nodiscard]] layerc::runtime::ServiceReconcileResult reconcile(
      const layerc::runtime::CapabilityDemand &demand) override;
  void quiesce() noexcept override;
  [[nodiscard]] layerc::runtime::RuntimeResourceStats
  resources() const noexcept override;

private:
  X11SessionRegistry &registry_;
};

} // namespace layerc::daemon
