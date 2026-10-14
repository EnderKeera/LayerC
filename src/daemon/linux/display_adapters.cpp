#include "display_adapters.hpp"
#include "display_selection.hpp"
#include "x11_adapters.hpp"

#include "layerc/core/utf8.hpp"

#ifdef LAYERC_HAVE_WAYLAND_CONNECTION
#include "wayland_connection.hpp"
#endif

#include <algorithm>
#include <cstdlib>

namespace layerc::daemon {
namespace {
namespace runtime = layerc::runtime;
using State = runtime::DisplayProviderState;

runtime::DisplayProviderStatus provider_from_capability(
    const LinuxStatusSnapshot &status, const runtime::Capability capability,
    const char *name, const char *fallback) {
  runtime::DisplayProviderStatus result{name, State::unavailable, {}};
  const auto row = std::ranges::find(status.capabilities, capability, &CapabilityStatus::capability);
  if (row != status.capabilities.end()) {
    switch (row->availability) {
    case CapabilityAvailability::active: result.state = State::active; break;
    case CapabilityAvailability::degraded: result.state = State::degraded; break;
    case CapabilityAvailability::inactive: result.state = State::inactive; break;
    case CapabilityAvailability::unavailable: break;
    }
    result.reasons = row->reasons;
  }
  if (fallback) {
    result.reasons.emplace_back(fallback);
    if (result.state == State::active) result.state = State::degraded;
  }
  return result;
}
void degrade_capability(LinuxStatusSnapshot &status, runtime::Capability capability,
                        const char *reason) {
  const auto row = std::ranges::find(status.capabilities, capability, &CapabilityStatus::capability);
  if (row == status.capabilities.end()) return;
  if (row->availability == CapabilityAvailability::active)
    row->availability = CapabilityAvailability::degraded;
  if (row->availability != CapabilityAvailability::inactive &&
      std::ranges::find(row->reasons, reason) == row->reasons.end())
    row->reasons.emplace_back(reason);
}
} // namespace

struct DisplayAdapters::Impl {
  // The existing implementation is the X11 provider group during the native
  // lanes' incremental integration, not a second host/connection.
  X11Adapters x11;
  runtime::DisplayStatusSnapshot display;
  bool x11_connected{};
  layerc::platform::EventLoop *loop{};
  int wayland_fd{-1};
#ifdef LAYERC_HAVE_WAYLAND_CONNECTION
  layerc::platform::WaylandConnection wayland;
#endif

  runtime::DisplaySession selected() const noexcept {
#ifdef LAYERC_HAVE_WAYLAND_CONNECTION
    const bool connected = wayland.status().connected;
#else
    const bool connected = false;
#endif
    return select_display_session(display.override_backend, connected, x11_connected);
  }
  void sync_connection() {
#ifdef LAYERC_HAVE_WAYLAND_CONNECTION
    const int next_fd = wayland.fd();
    if (wayland_fd >= 0 && next_fd != wayland_fd) loop->unregister_fd(wayland_fd);
    wayland_fd = next_fd;
    if (wayland_fd >= 0) loop->register_fd(wayland_fd, wayland.poll_events());
#endif
  }
  DisplayDispatch dispatch_x11(std::span<const int> ready, bool pending) {
    const bool enabled = selected() != runtime::DisplaySession::none;
    if (!enabled) x11.stop_sessions(layerc::core::MonotonicClock::now());
    // The connection can outlive an explicitly selected Wayland session.
    // Always drain its queue/fd, but suppress work from an unselected provider.
    const auto result = pending ? x11.dispatch_pending() : x11.dispatch(ready);
    if (enabled) return result;
    DisplayDispatch drained;
    drained.events = result.events;
    return drained;
  }
  bool dispatch_wayland() {
#ifdef LAYERC_HAVE_WAYLAND_CONNECTION
    const bool changed = wayland.dispatch();
    sync_connection();
    return changed;
#else
    return false;
#endif
  }
  void publish(LinuxStatusSnapshot &status) const {
    auto value = display;
#ifdef LAYERC_HAVE_WAYLAND_CONNECTION
    value.wayland = wayland.status();
#endif
    value.session = selected();
    if (value.session == runtime::DisplaySession::wayland)
      value.compositor = detect_compositor(value.desktop, false, false,
          value.wayland.globals.contains("zwlr_layer_shell_v1"),
          value.wayland.globals.contains("zwlr_foreign_toplevel_manager_v1"));
    else
      value.compositor = runtime::DisplayCompositor::unknown;

    if (value.session == runtime::DisplaySession::none) {
      const auto missing = [](const char *name, const char *reason) {
        return runtime::DisplayProviderStatus{name, State::not_built, {reason}};
      };
      if (value.override_backend == runtime::DisplayBackend::x11) {
#ifdef LAYERC_NO_X11
        value.providers.windows = missing("x11_ewmh", "x11_not_built");
        value.providers.monitors = missing("x11_randr", "x11_not_built");
        value.providers.input = missing("x11_xi2", "x11_not_built");
        value.providers.overlay = missing("x11_shape", "x11_not_built");
        value.providers.capture = missing("x11_composite", "x11_not_built");
#else
        const runtime::DisplayProviderStatus absent{"none", State::unavailable, {status.x11.reason}};
        value.providers = {absent, absent, absent, absent, absent};
#endif
      } else {
        // These native provider lanes are not yet wired. Do not advertise
        // support solely because the Wayland transport was compiled.
        value.providers.windows = missing("wlr_foreign_toplevel", "native_provider_not_built");
        value.providers.monitors = missing("wayland_output", "native_provider_not_built");
        value.providers.input = missing("evdev", "input_provider_not_integrated");
        value.providers.overlay = missing("wlr_layer_shell", "native_provider_not_built");
        value.providers.capture = missing("portal_screencast", "native_provider_not_built");
      }
      value.reasons.emplace_back(value.override_backend == runtime::DisplayBackend::invalid
          ? "invalid_display_backend" : "session_unavailable");
    } else if (status.x11.connected) {
      const bool fallback = value.session == runtime::DisplaySession::wayland;
      value.providers.windows = {"x11_ewmh", State::active, {}};
      value.providers.monitors = {"x11_randr", State::active, {}};
      if (!status.x11.ewmh) {
        value.providers.windows.state = State::degraded;
        value.providers.windows.reasons.emplace_back("ewmh_missing");
      }
      if (!status.x11.randr.available) {
        value.providers.monitors.state = State::degraded;
        value.providers.monitors.reasons.emplace_back("randr_missing");
      }
      if (fallback) {
        value.providers.windows.state = value.providers.monitors.state = State::degraded;
        value.providers.windows.reasons.emplace_back("xwayland_windows_only");
        value.providers.monitors.reasons.emplace_back("xwayland_monitors");
      }
      value.providers.input = provider_from_capability(status, runtime::Capability::input,
          "x11_xi2", fallback ? "xwayland_focus_only" : nullptr);
      value.providers.overlay = provider_from_capability(status, runtime::Capability::overlay,
          "x11_shape", fallback ? "xwayland_overlay" : nullptr);
      value.providers.capture = provider_from_capability(status, runtime::Capability::visual,
          "x11_composite", fallback ? "xwayland_windows_only" : nullptr);
      if (fallback) {
        degrade_capability(status, runtime::Capability::input, "xwayland_focus_only");
        degrade_capability(status, runtime::Capability::overlay, "xwayland_overlay");
        degrade_capability(status, runtime::Capability::visual, "xwayland_windows_only");
      }
    } else {
      value.providers.windows = {"wlr_foreign_toplevel", State::not_built, {"native_provider_not_built"}};
      value.providers.monitors = {"wayland_output", State::not_built, {"native_provider_not_built"}};
      value.providers.input = {"evdev", State::not_built, {"input_provider_not_integrated"}};
      value.providers.overlay = {"wlr_layer_shell", State::not_built, {"native_provider_not_built"}};
      value.providers.capture = {"portal_screencast", State::not_built, {"native_provider_not_built"}};
    }
    status.display = std::move(value);
  }
};

DisplayAdapters::DisplayAdapters() : impl_(std::make_unique<Impl>()) {}
DisplayAdapters::~DisplayAdapters() = default;
layerc::runtime::RuntimeServiceRegistry DisplayAdapters::runtime_services() noexcept {
  return impl_->x11.runtime_services();
}
void DisplayAdapters::start(layerc::platform::EventLoop &loop, LinuxStatusSnapshot &status,
                            std::vector<std::string> &diagnostics) {
  impl_->loop = &loop;
  auto &display = impl_->display;
  const auto *backend = std::getenv("LAYERC_DISPLAY_BACKEND");
  display.override_backend = display_backend(backend ? std::string_view{backend} : std::string_view{});
  const auto *desktop = std::getenv("XDG_CURRENT_DESKTOP");
  if (desktop) {
    const std::string_view value{desktop};
    if (value.size() <= 512 && layerc::core::is_valid_utf8(value)) display.desktop = value;
    else display.reasons.emplace_back("invalid_desktop");
  }
  if (display.override_backend != runtime::DisplayBackend::x11 &&
      display.override_backend != runtime::DisplayBackend::invalid) {
#ifdef LAYERC_HAVE_WAYLAND_CONNECTION
    (void)impl_->wayland.open();
    impl_->sync_connection();
#else
    display.wayland.reason = "wayland_not_built";
#endif
  }
  if (display.override_backend == runtime::DisplayBackend::auto_detect ||
      display.override_backend == runtime::DisplayBackend::x11 ||
      impl_->selected() == runtime::DisplaySession::wayland) {
    impl_->x11.start(loop, status, diagnostics);
    impl_->x11_connected = status.x11.connected;
  } else {
    status.x11.reason = "backend_not_selected";
  }
  impl_->publish(status);
}
void DisplayAdapters::refresh(std::span<const layerc::platform::StoredProfile> profiles,
    bool configuration_changed, layerc::runtime::DaemonKernel &kernel,
    LinuxStatusSnapshot &status, std::vector<std::string> &diagnostics) {
  if (impl_->selected() == runtime::DisplaySession::none) {
    impl_->x11.stop_sessions(layerc::core::MonotonicClock::now());
    impl_->x11.refresh({}, true, kernel, status, diagnostics);
  } else {
    impl_->x11.refresh(profiles, configuration_changed, kernel, status, diagnostics);
  }
  impl_->publish(status);
}
void DisplayAdapters::refresh_status(const layerc::runtime::DaemonKernel &kernel,
                                     LinuxStatusSnapshot &status) const {
  impl_->x11.refresh_status(kernel, status);
  impl_->publish(status);
}
DisplayDispatch DisplayAdapters::dispatch(std::span<const int> ready) {
  const bool changed = impl_->dispatch_wayland();
  auto result = impl_->dispatch_x11(ready, false);
  result.windows_changed = result.windows_changed || changed;
  return result;
}
bool DisplayAdapters::has_pending_events() {
  bool pending = impl_->x11.has_pending_events();
#ifdef LAYERC_HAVE_WAYLAND_CONNECTION
  pending = impl_->wayland.has_pending_events() || pending;
  impl_->sync_connection();
#endif
  return pending;
}
DisplayDispatch DisplayAdapters::dispatch_pending() {
  const bool changed = impl_->dispatch_wayland();
  auto result = impl_->dispatch_x11({}, true);
  result.windows_changed = result.windows_changed || changed;
  return result;
}
bool DisplayAdapters::render(layerc::core::TimePoint now) {
  return impl_->selected() != runtime::DisplaySession::none && impl_->x11.render(now);
}
void DisplayAdapters::stop_sessions(layerc::core::TimePoint now) { impl_->x11.stop_sessions(now); }
std::optional<layerc::core::TimePoint> DisplayAdapters::next_deadline(layerc::core::TimePoint now) const {
  return impl_->x11.next_deadline(now);
}
std::size_t DisplayAdapters::session_count() const noexcept { return impl_->x11.session_count(); }
std::vector<layerc::runtime::SessionDiagnostics> DisplayAdapters::session_diagnostics() const {
  return impl_->x11.session_diagnostics();
}
TraceControlResult DisplayAdapters::trace_request(const layerc::core::TraceControlRequest &request) {
  return impl_->x11.trace_request(request);
}
} // namespace layerc::daemon
