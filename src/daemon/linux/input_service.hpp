#pragma once

#include "x11_adapters.hpp"
#include "x11_display.hpp"
#include "x11_input.hpp"
#include "x11_window.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/input_matcher.hpp"
#include "layerc/core/pixel_engine.hpp"
#include "layerc/core/automation_session.hpp"
#include "layerc/platform/automation_persistence.hpp"
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

// References into one loaded registry revision. Visual assets can be prepared
// before a profile has a matching window.
struct VisualConfiguration {
  const std::filesystem::path &source_path;
  const std::string &profile_id;
  const layerc::core::VisualProfile &profile;
};

// One configured profile revision: the base profile plus its module
// sidecars as value copies (the platform's shared ModuleSidecar).
struct ConfiguredProfile {
  layerc::platform::StoredProfile source;
  layerc::platform::ProfileModuleSidecars modules;

  // Sidecars whose definitions are enabled; invalid ones contribute nothing.
  [[nodiscard]] const std::optional<layerc::core::VisualProfile> &
  visual_profile() const noexcept {
    return modules.get<layerc::core::VisualModule>().profile;
  }
  [[nodiscard]] const std::optional<layerc::core::AutomationProfile> &
  automation_profile() const noexcept {
    return modules.get<layerc::core::AutomationModule>().profile;
  }
  [[nodiscard]] bool invalid_visual() const noexcept {
    return modules.get<layerc::core::VisualModule>().state ==
           layerc::platform::ModuleSidecarState::invalid;
  }
  [[nodiscard]] bool invalid_automation() const noexcept {
    return modules.get<layerc::core::AutomationModule>().state ==
           layerc::platform::ModuleSidecarState::invalid;
  }
};

// One matched profile owns cooldown, automation, and visual state. Direct
// bindings are optional; bindings the X11 adapter cannot produce (controller
// or HID tokens) stay in the matcher and simply never fire. Persistent
// automation values belong to the registry's AutomationPersistence, which
// outlives the session.
struct InputSession {
  layerc::core::AutomationSession automation;
  std::filesystem::path source_path;
  std::uint64_t window{};
  bool global{};
  bool active{};
  layerc::core::InputMatcher matcher;
  layerc::platform::ProfileModuleSidecars modules;
  [[nodiscard]] std::optional<layerc::core::VisualProfile> &
  visual_profile() noexcept {
    return modules.get<layerc::core::VisualModule>().profile;
  }
  [[nodiscard]] const std::optional<layerc::core::VisualProfile> &
  visual_profile() const noexcept {
    return modules.get<layerc::core::VisualModule>().profile;
  }
  bool invalid_automation_sidecar{};
  // "<device>:<code>:press|release" of the last event a binding consumed,
  // as the Windows daemon reports it.
  std::string last_input_match;
  layerc::platform::AutomationPersistence *persistence{};

  InputSession(const ConfiguredProfile &bundle, std::uint64_t window,
               layerc::platform::AutomationPersistence &persistence);

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
  // Runs session_stopping and hands the final values to the persistence
  // owner, which writes them now and keeps retrying on failure.
  void stop_automation(layerc::core::TimePoint now);
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
  // Unsaved values of profiles that no longer have a session.
  [[nodiscard]] std::size_t pending_checkpoint_count() const;
  [[nodiscard]] const layerc::platform::AutomationPersistence &
  automation_persistence() const noexcept { return persistence_; }
  // True when a pending checkpoint completes and capability demand changes.
  [[nodiscard]] bool poll_automation(layerc::core::TimePoint now);
  // Capability quiesce: write every pending value without ending sessions.
  void checkpoint_automation(layerc::core::TimePoint now);
  [[nodiscard]] std::optional<layerc::core::TimePoint>
  next_automation_deadline(layerc::core::TimePoint now);
  [[nodiscard]] std::vector<std::string> automation_diagnostics() const;
  [[nodiscard]] std::span<const std::string> visual_diagnostics() const {
    return visual_diagnostics_;
  }

private:
  // Marks runtimes whose values the persistence owner just made durable.
  void acknowledge_saved(std::span<const std::filesystem::path> saved);

  std::vector<ConfiguredProfile> configured_;
  std::vector<VisualConfiguration> visual_configurations_;
  std::vector<InputSession> sessions_;
  layerc::platform::AutomationPersistence persistence_;
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

  // The shared dispatcher observes discovery before decoding passive input,
  // so neither consumer drains events unseen by the other.
  [[nodiscard]] X11Dispatch dispatch(
      layerc::platform::X11WindowTracker *tracker = nullptr);

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
