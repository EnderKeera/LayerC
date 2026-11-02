#include "session_engine.hpp"
#include "trace_lease_host.hpp"

#include "input_service.hpp"
#include "overlay_service.hpp"
#include "visual_service.hpp"

#include "layerc/core/panel_presentation.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>

namespace layerc::daemon {


struct SessionEngine::Impl {
  SessionRegistry sessions;
  bool native_input{};
  TraceLeaseHost trace;
  InputService input{sessions};
  AutomationService automation{sessions};
  OverlayService overlay;
  VisualService visual;
  DisplayRoute route;

  [[nodiscard]] std::vector<std::string> overlay_faults() const {
    std::vector<std::string> result;
    for (const auto &session : std::as_const(sessions).sessions()) {
      std::optional<layerc::core::AutomationRuntimeSnapshot> snapshot;
      if (session.automation.runtime())
        snapshot = session.automation.runtime()->snapshot();
      const auto *automation = snapshot ? &*snapshot
          : (session.presentation_defaults ? &*session.presentation_defaults
                                           : nullptr);
      auto lines = layerc::core::overlay_item_diagnostics(
          session.automation.profile(), automation,
          layerc::core::MonotonicClock::now());
      result.insert(result.end(), std::make_move_iterator(lines.begin()),
                    std::make_move_iterator(lines.end()));
    }
    return result;
  }
};

SessionEngine::SessionEngine() : impl_(std::make_unique<Impl>()) {}
SessionEngine::~SessionEngine() = default;

layerc::runtime::RuntimeServiceRegistry
SessionEngine::runtime_services() noexcept {
  layerc::runtime::RuntimeServiceRegistry result{};
  result[static_cast<std::size_t>(layerc::runtime::Capability::input)] =
      &impl_->input;
  result[static_cast<std::size_t>(layerc::runtime::Capability::automation)] =
      &impl_->automation;
  result[static_cast<std::size_t>(layerc::runtime::Capability::overlay)] =
      &impl_->overlay;
  result[static_cast<std::size_t>(layerc::runtime::Capability::visual)] =
      &impl_->visual;
  return result;
}

void SessionEngine::release_input() noexcept { impl_->input.release(); }

InputDeviceNeeds SessionEngine::input_needs() const {
  return needed_devices(impl_->sessions.sessions());
}

DisplayDispatch SessionEngine::poll_input(const std::span<const int> ready) {
  return impl_->input.poll(ready);
}

void SessionEngine::refresh(
    const std::span<const layerc::platform::StoredProfile> profiles,
    const bool configuration_changed, const DisplayRoute &route,
    layerc::runtime::DaemonKernel &kernel, LinuxStatusSnapshot &status,
    std::vector<std::string> &diagnostics) {
  if (configuration_changed) impl_->sessions.configure(profiles);
  impl_->route = route;
  impl_->input.use_sources(route.inputs);
  impl_->native_input = route.native_input;
  impl_->overlay.use_backend(route.overlay);
  impl_->visual.use_route(route.capture);
  // Losing a provider retires its sessions through the normal checkpoint
  // path. Session-less failed writes still contribute automation demand.
  const auto scan = route.windows ? route.windows->windows()
                                  : layerc::platform::WindowScan{};
  impl_->sessions.refresh(scan);
  // New runtimes start untraced: re-apply the live subscribers immediately.
  (void)impl_->trace.expire(layerc::core::MonotonicClock::now(),
                            impl_->sessions.sessions());
  const auto monitors = route.monitors ? route.monitors->monitors()
                                        : layerc::platform::MonitorScan{};
  impl_->overlay.refresh(impl_->sessions.sessions(), scan, monitors);
  const auto requests = impl_->sessions.session_requests();
  const auto demand =
      layerc::runtime::calculate_capability_demand(requests, false);
  // Capture resolution reasons live in the visual service, which owns the
  // route: every session is offered and an unpollable target reports itself.
  impl_->visual.refresh(impl_->sessions.sessions(), demand,
                        configuration_changed,
                        impl_->sessions.visual_configurations());
  (void)kernel.reconcile(demand);

  const auto input_unavailable = impl_->input.unavailable();
  const auto overlay_unavailable = impl_->overlay.unavailable();
  const auto automation_unavailable = impl_->sessions.automation_diagnostics();
  auto visual_unavailable = impl_->visual.unavailable();
  visual_unavailable.insert(visual_unavailable.end(),
                            impl_->sessions.visual_diagnostics().begin(),
                            impl_->sessions.visual_diagnostics().end());
  derive_capability_status(
      status, demand,
      {.input = input_unavailable,
       .overlay = overlay_unavailable,
       .automation = automation_unavailable,
       .visual = visual_unavailable,
       .native_overlay = route.native_overlay,
       .native_input = impl_->native_input});
  for (const auto &reason : input_unavailable)
    diagnostics.push_back("input " + reason);
  for (const auto &reason : overlay_unavailable)
    diagnostics.push_back("overlay " + reason);
  for (const auto &reason : automation_unavailable)
    diagnostics.push_back("automation " + reason);
  for (const auto &reason : visual_unavailable)
    diagnostics.push_back("visual " + reason);
  for (const auto name : unavailable_capabilities(kernel.demand(),
                                                   kernel.services()))
    diagnostics.push_back("capability " + std::string{name} +
                          " demanded but unavailable");
  status.overlay_faults = impl_->overlay_faults();
  for (const auto &line : status.overlay_faults)
    diagnostics.push_back(line);
}

void SessionEngine::refresh_status(
    const layerc::runtime::DaemonKernel &kernel,
    LinuxStatusSnapshot &status) const {
  const auto &route = impl_->route;
  auto visual_unavailable = impl_->visual.unavailable();
  visual_unavailable.insert(visual_unavailable.end(),
                            impl_->sessions.visual_diagnostics().begin(),
                            impl_->sessions.visual_diagnostics().end());
  derive_capability_status(
      status, kernel.demand(),
      {.input = impl_->input.unavailable(),
       .overlay = impl_->overlay.unavailable(),
       .automation = impl_->sessions.automation_diagnostics(),
       .visual = visual_unavailable,
       .native_overlay = route.native_overlay,
       .native_input = impl_->native_input});
  status.overlay_faults = impl_->overlay_faults();
}

void SessionEngine::feed_input(const std::span<const layerc::core::InputEvent> events,
                                DisplayDispatch &result) {
  impl_->input.feed(events, result);
}

bool SessionEngine::render(const layerc::core::TimePoint now) {
  (void)impl_->visual.poll(impl_->sessions.sessions(), now);
  // Trace leases keep the engine's timer honest; a trace expiry is not itself a
  // capability-demand change, so its flag is ignored here.
  (void)impl_->trace.expire(now, impl_->sessions.sessions());
  const bool demand_changed = impl_->sessions.poll_automation(now);
  impl_->overlay.render(impl_->sessions.sessions(), now);
  return demand_changed;
}

void SessionEngine::stop_sessions(const layerc::core::TimePoint now) {
  impl_->sessions.stop_sessions(now);
}

std::optional<layerc::core::TimePoint>
SessionEngine::next_deadline(const layerc::core::TimePoint now) const {
  auto next = impl_->overlay.next_render_deadline(now);
  next = layerc::core::earliest_deadline(
      next, impl_->sessions.next_automation_deadline(now));
  next = layerc::core::earliest_deadline(next, impl_->trace.next_deadline());
  return layerc::core::earliest_deadline(next,
                                         impl_->visual.next_deadline(now));
}

std::size_t SessionEngine::session_count() const noexcept {
  return impl_->sessions.session_count();
}

std::vector<layerc::runtime::SessionDiagnostics>
SessionEngine::session_diagnostics() const {
  std::vector<layerc::runtime::SessionDiagnostics> rows;
  const auto sessions = std::as_const(impl_->sessions).sessions();
  rows.reserve(sessions.size());
  for (const auto &session : sessions)
    rows.push_back({
        .profile_id = session.automation.profile().id,
        .source_key = layerc::platform::canonical_profile_key(
            session.source_path),
        .window = session.window,
        .foreground = session.active,
        .overlay_windows = static_cast<std::uint32_t>(std::min<std::size_t>(
            impl_->overlay.window_count(session.automation.profile().id, session.window),
            0xFFFFFFFFU)),
        .last_input_match = session.last_input_match,
    });
  return rows;
}

TraceControlResult SessionEngine::trace_request(const layerc::core::TraceControlRequest &request) {
  return impl_->trace.handle(request, impl_->sessions.sessions(),
                             layerc::core::MonotonicClock::now());
}

} // namespace layerc::daemon
