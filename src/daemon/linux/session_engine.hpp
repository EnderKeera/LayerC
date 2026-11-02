#pragma once

#include "capture_route.hpp"
#include "daemon_host.hpp"
#include "display_providers.hpp"
#include "display_dispatch.hpp"
#include "display_selection.hpp"
#include "event_loop.hpp"

#include "layerc/platform/profile_store.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace layerc::daemon {

// Selected provider pointers bound from the resolved route plan.
struct DisplayRoute {
  DisplaySource *windows{};
  DisplaySource *monitors{};
  OverlayBackend *overlay{};
  CaptureRoute capture;
  bool native_overlay{};
  std::vector<PassiveInputSource *> inputs; // ordered; fallback last
  bool native_input{};
};

// Backend-neutral owner of sessions and demand-driven runtime services.
class SessionEngine {
public:
  SessionEngine();
  ~SessionEngine();
  SessionEngine(const SessionEngine &) = delete;
  SessionEngine &operator=(const SessionEngine &) = delete;

  [[nodiscard]] layerc::runtime::RuntimeServiceRegistry
  runtime_services() noexcept;
  // Closes every borrowed passive source; used before the owner reconfigures one.
  void release_input() noexcept;
  [[nodiscard]] InputDeviceNeeds input_needs() const;
  // Drains the attached passive source that owns descriptors (none for XI2).
  [[nodiscard]] DisplayDispatch poll_input(std::span<const int> ready);
  void refresh(std::span<const layerc::platform::StoredProfile> profiles,
               bool configuration_changed, const DisplayRoute &route,
               layerc::runtime::DaemonKernel &kernel,
               LinuxStatusSnapshot &status,
               std::vector<std::string> &diagnostics);
  void refresh_status(const layerc::runtime::DaemonKernel &kernel,
                      LinuxStatusSnapshot &status) const;
  void feed_input(std::span<const layerc::core::InputEvent> events,
                  DisplayDispatch &result);
  // True when a completed retired checkpoint changes automation demand.
  [[nodiscard]] bool render(layerc::core::TimePoint now);
  void stop_sessions(layerc::core::TimePoint now);
  [[nodiscard]] std::optional<layerc::core::TimePoint>
  next_deadline(layerc::core::TimePoint now) const;
  [[nodiscard]] std::size_t session_count() const noexcept;
  [[nodiscard]] std::vector<layerc::runtime::SessionDiagnostics>
  session_diagnostics() const;
  [[nodiscard]] TraceControlResult trace_request(const layerc::core::TraceControlRequest &request);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace layerc::daemon
