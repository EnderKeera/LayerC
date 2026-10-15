#include "display_adapters.hpp"
#include "display_selection.hpp"
#include "x11_adapters.hpp"
#ifdef LAYERC_HAVE_BUS_PROVIDERS
#include "bus_providers.hpp"
#endif

#include "layerc/core/utf8.hpp"

#ifdef LAYERC_HAVE_WAYLAND_CONNECTION
#include "wayland_connection.hpp"
#endif
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
#include "wayland_providers.hpp"
#endif

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <utility>

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
const char *input_opt_in_reason(const InputSelection selection) noexcept {
  return selection == InputSelection::invalid ? "invalid_input_backend" : "evdev_not_enabled";
}

runtime::DisplayProviderStatus evdev_provider(const InputRouteStatus &route,
                                              const InputSelection selection) {
  if (!route.built) return {"evdev", State::not_built, {"input_provider_not_built"}};
  if (selection != InputSelection::evdev)
    return {"evdev", State::unavailable, {input_opt_in_reason(selection)}};
  if (route.evdev_attached)
    return {"evdev", route.evdev_reasons.empty() ? State::active : State::degraded,
            route.evdev_reasons};
  if (!route.evdev_reasons.empty()) return {"evdev", State::unavailable, route.evdev_reasons};
  return {"evdev", State::inactive, {"not_demanded"}};
}
} // namespace

struct DisplayAdapters::Impl {
  // The existing implementation is the X11 provider group during the native
  // lanes' incremental integration, not a second host/connection.
  X11Adapters x11;
  runtime::DisplayStatusSnapshot display;
  bool x11_connected{};
  InputOptions input;
  layerc::platform::EventLoop *loop{};
  int wayland_fd{-1};
#ifdef LAYERC_HAVE_WAYLAND_CONNECTION
  layerc::platform::WaylandConnection wayland;
#endif
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
  // Declared after the connection: destroyed first, while it is valid.
  std::unique_ptr<WaylandProviders> native;
  bool native_dirty{};
#endif
#ifdef LAYERC_HAVE_BUS_PROVIDERS
  std::unique_ptr<BusProviders> bus;
#endif

  ~Impl() {
    x11.set_native(nullptr);
#ifdef LAYERC_HAVE_BUS_PROVIDERS
    bus.reset();
#endif
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
    native.reset();
#endif
  }
  void sync_native() {
    NativeDisplaySource *source{};
    if (selected() == runtime::DisplaySession::wayland) {
#ifdef LAYERC_HAVE_BUS_PROVIDERS
      source = bus.get();
#endif
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
      if (!source) source = native.get();
#endif
    }
    x11.set_native(source);
  }
  bool dispatch_bus(std::span<const int> ready) {
#ifdef LAYERC_HAVE_BUS_PROVIDERS
    if (bus) {
      if (selected() != runtime::DisplaySession::wayland)
        bus->configure(false, display.desktop, false);
      sync_native();
      return bus->dispatch(ready);
    }
#else
    (void)ready;
#endif
    sync_native();
    return false;
  }

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
    bool changed = wayland.dispatch();
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
    if (native) {
      if (changed) native->sync();
      if (native->take_keymap_changed()) x11.set_input_keymap(native->keymap());
      // Evaluate every flag so none survives to cause a spurious refresh.
      const bool dirty = std::exchange(native_dirty, false);
      changed = native->take_changed() || dirty || changed;
      native->repaint();
    }
#endif
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
    const auto route = x11.input_route();
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
    const bool native_session = native && value.session == runtime::DisplaySession::wayland;
    bool native_overlay = native_session && native->overlay_provider().state != State::unavailable;
#else
    bool native_overlay = false;
#endif
#ifdef LAYERC_HAVE_BUS_PROVIDERS
    const bool bus_session = bus && value.session == runtime::DisplaySession::wayland;
    native_overlay = native_overlay || (bus_session && bus->overlay());
    const bool native_capture = bus_session && bus->capture_demanded();
#else
    const bool native_capture = false;
#endif
    if (value.session == runtime::DisplaySession::wayland)
      value.compositor = detect_compositor(value.desktop,
#ifdef LAYERC_HAVE_BUS_PROVIDERS
          bus_session && bus->kwin_owner(), bus_session && bus->gnome_owner(),
#else
          false, false,
#endif
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
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
        // The native lane is built; without a Wayland session it is
        // unavailable for the session's reason, not missing from the build.
        const auto lost = [&](const char *name) {
          return runtime::DisplayProviderStatus{
              name, State::unavailable,
              {value.wayland.reason.empty() ? std::string{"session_unavailable"}
                                            : value.wayland.reason}};
        };
        value.providers.windows = lost("wlr_foreign_toplevel");
        value.providers.monitors = lost("wayland_output");
        value.providers.overlay = lost("wlr_layer_shell");
#else
        // Do not advertise support solely because the transport was compiled.
        value.providers.windows = missing("wlr_foreign_toplevel", "native_provider_not_built");
        value.providers.monitors = missing("wayland_output", "native_provider_not_built");
        value.providers.overlay = missing("wlr_layer_shell", "native_provider_not_built");
#endif
        // An attached route still reports its own state without a session.
        if (route.built && input.selection == InputSelection::evdev && !route.evdev_attached)
          value.providers.input = {"evdev", State::unavailable, {"session_unavailable"}};
        else
          value.providers.input = evdev_provider(route, input.selection);
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
      // The selected evdev route serves input unless XI2 took over after it
      // failed; that fallback keeps evdev's reason visible.
      const bool evdev = route.evdev_selected && (route.evdev_attached || !route.xi2_attached);
      if (evdev) {
        value.providers.input = evdev_provider(route, input.selection);
      } else {
        value.providers.input = provider_from_capability(status, runtime::Capability::input,
            "x11_xi2", fallback ? "xwayland_focus_only" : nullptr);
        if (fallback) {
          auto &reasons = value.providers.input.reasons;
          if (input.selection == InputSelection::evdev)
            reasons.insert(reasons.end(), route.evdev_reasons.begin(), route.evdev_reasons.end());
          else
            reasons.emplace_back(input_opt_in_reason(input.selection));
        }
      }
      if (!native_overlay)
        value.providers.overlay = provider_from_capability(status, runtime::Capability::overlay,
            "x11_shape", fallback ? "xwayland_overlay" : nullptr);
      value.providers.capture = provider_from_capability(status, runtime::Capability::visual,
          "x11_composite", fallback ? "xwayland_windows_only" : nullptr);
      if (fallback) {
        if (!evdev) degrade_capability(status, runtime::Capability::input, "xwayland_focus_only");
        if (!native_overlay)
          degrade_capability(status, runtime::Capability::overlay, "xwayland_overlay");
        if (!native_capture)
          degrade_capability(status, runtime::Capability::visual, "xwayland_windows_only");
      }
    } else {
      value.providers.windows = {"wlr_foreign_toplevel", State::not_built, {"native_provider_not_built"}};
      value.providers.monitors = {"wayland_output", State::not_built, {"native_provider_not_built"}};
      value.providers.input = evdev_provider(route, input.selection);
      value.providers.overlay = {"wlr_layer_shell", State::not_built, {"native_provider_not_built"}};
      value.providers.capture = {"portal_screencast", State::not_built, {"native_provider_not_built"}};
    }
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
    if (native_session) {
      // Native providers win; an unusable one keeps its XWayland fallback
      // when there is one, otherwise its own reason.
      const bool fallback = status.x11.connected;
      // XWayland EWMH keeps windows while connected: foreign toplevels have
      // no PID, so executable-matched profiles could never attach.
      if (!fallback) value.providers.windows = native->window_provider();
      if (auto monitors = native->monitor_provider(); monitors.state != State::unavailable || !fallback)
        value.providers.monitors = std::move(monitors);
      if (native_overlay)
        value.providers.overlay =
            provider_from_capability(status, runtime::Capability::overlay, "wlr_layer_shell", nullptr);
      else if (!fallback)
        value.providers.overlay = native->overlay_provider();
    }
#endif
#ifdef LAYERC_HAVE_BUS_PROVIDERS
    if (bus_session) {
      const bool fallback = status.x11.connected;
      if (auto windows = bus->window_provider()) {
        if (bus->prefer_windows() || !fallback) value.providers.windows = std::move(*windows);
        else if (windows->state == State::unavailable) {
          auto &reasons = value.providers.windows.reasons;
          reasons.insert(reasons.end(), windows->reasons.begin(), windows->reasons.end());
        }
      }
      if (auto overlay = bus->overlay_provider(); overlay &&
          (bus->overlay() || !fallback || overlay->state == State::inactive))
        value.providers.overlay = std::move(*overlay);
      if (native_capture || !fallback) value.providers.capture = bus->capture_provider();
    }
#endif
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
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
    if (impl_->wayland.status().connected)
      impl_->native = std::make_unique<WaylandProviders>(impl_->wayland);
#endif
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
#ifdef LAYERC_HAVE_BUS_PROVIDERS
  if (impl_->selected() == runtime::DisplaySession::wayland) {
    NativeDisplaySource *fallback{};
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
    fallback = impl_->native.get();
#endif
    impl_->bus = std::make_unique<BusProviders>(loop, fallback);
  }
#endif
  impl_->sync_native();
  impl_->x11.prefer_evdev(impl_->selected() == runtime::DisplaySession::wayland);
  if (impl_->input.selection == InputSelection::invalid)
    display.reasons.emplace_back("invalid_input_backend");
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
  if (impl_->native && impl_->selected() == runtime::DisplaySession::wayland) {
    (void)impl_->native->take_keymap_changed();
    impl_->x11.set_input_keymap(impl_->native->keymap());
  }
#endif
  impl_->publish(status);
}
void DisplayAdapters::configure_input(const InputOptions &options) {
  impl_->input = options;
  impl_->x11.configure_input(options);
}
void DisplayAdapters::refresh(std::span<const layerc::platform::StoredProfile> profiles,
    bool configuration_changed, layerc::runtime::DaemonKernel &kernel,
    LinuxStatusSnapshot &status, std::vector<std::string> &diagnostics) {
#ifdef LAYERC_HAVE_BUS_PROVIDERS
  if (impl_->bus) {
    const bool tracking = impl_->selected() == runtime::DisplaySession::wayland &&
        std::ranges::any_of(profiles, [](const auto &profile) { return profile.profile.enabled; });
    impl_->bus->configure(tracking, impl_->display.desktop, configuration_changed);
  }
#endif
  impl_->sync_native();
  if (impl_->selected() == runtime::DisplaySession::none) {
    impl_->x11.stop_sessions(layerc::core::MonotonicClock::now());
    impl_->x11.refresh({}, true, kernel, status, diagnostics);
  } else {
    impl_->x11.refresh(profiles, configuration_changed, kernel, status, diagnostics);
  }
#ifdef LAYERC_HAVE_BUS_PROVIDERS
  if (impl_->bus) impl_->bus->want_overlay(kernel.demand().required(runtime::Capability::overlay));
#endif
  impl_->publish(status);
}
void DisplayAdapters::refresh_status(const layerc::runtime::DaemonKernel &kernel,
                                     LinuxStatusSnapshot &status) const {
  impl_->x11.refresh_status(kernel, status);
  impl_->publish(status);
}
DisplayDispatch DisplayAdapters::dispatch(std::span<const int> ready) {
  const bool wayland_changed = impl_->dispatch_wayland();
  const bool changed = impl_->dispatch_bus(ready) || wayland_changed;
  auto result = impl_->dispatch_x11(ready, false);
  result.windows_changed = result.windows_changed || changed;
  return result;
}
bool DisplayAdapters::has_pending_events() {
  bool pending = impl_->x11.has_pending_events();
#ifdef LAYERC_HAVE_WAYLAND_CONNECTION
  pending = impl_->wayland.has_pending_events() || pending;
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
  if (impl_->native && impl_->native->take_changed()) {
    impl_->native_dirty = true;
    pending = true;
  }
#endif
  impl_->sync_connection();
#endif
#ifdef LAYERC_HAVE_BUS_PROVIDERS
  if (impl_->bus) {
    const auto deadline = impl_->bus->next_deadline();
    pending = pending || (deadline && *deadline <= layerc::core::MonotonicClock::now());
  }
#endif
  return pending;
}
DisplayDispatch DisplayAdapters::dispatch_pending() {
  const bool wayland_changed = impl_->dispatch_wayland();
  const bool changed = impl_->dispatch_bus({}) || wayland_changed;
  auto result = impl_->dispatch_x11({}, true);
  result.windows_changed = result.windows_changed || changed;
  return result;
}
bool DisplayAdapters::render(layerc::core::TimePoint now) {
  const bool changed = impl_->selected() != runtime::DisplaySession::none && impl_->x11.render(now);
#ifdef LAYERC_HAVE_BUS_PROVIDERS
  if (impl_->bus) impl_->bus->present();
#endif
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
  if (impl_->native) {
    // Overlay commits queue on the connection; send them now.
    impl_->wayland.flush();
    impl_->sync_connection();
  }
#endif
  return changed;
}
void DisplayAdapters::stop_sessions(layerc::core::TimePoint now) {
  impl_->x11.stop_sessions(now);
#ifdef LAYERC_HAVE_BUS_PROVIDERS
  if (impl_->bus) impl_->bus->stop();
#endif
}
std::optional<layerc::core::TimePoint> DisplayAdapters::next_deadline(layerc::core::TimePoint now) const {
  auto next = impl_->x11.next_deadline(now);
#ifdef LAYERC_HAVE_BUS_PROVIDERS
  if (impl_->bus) next = layerc::core::earliest_deadline(next, impl_->bus->next_deadline());
#endif
  return next;
}
std::size_t DisplayAdapters::session_count() const noexcept { return impl_->x11.session_count(); }
std::vector<layerc::runtime::SessionDiagnostics> DisplayAdapters::session_diagnostics() const {
  return impl_->x11.session_diagnostics();
}
TraceControlResult DisplayAdapters::trace_request(const layerc::core::TraceControlRequest &request) {
  return impl_->x11.trace_request(request);
}
} // namespace layerc::daemon
