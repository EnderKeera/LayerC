#include "display_adapters.hpp"
#include "display_selection.hpp"
#include "display_route.hpp"
#include "evdev_route.hpp"
#include "session_engine.hpp"
#ifdef LAYERC_HAVE_X11
#include "x11_provider.hpp"
#endif
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
#ifdef LAYERC_HAVE_X11
  X11Provider x11;
#endif
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

  // Borrowed by the engine's input service; declared first so the engine
  // releases it before destruction.
  EvdevInputRoute evdev;
  // Services release provider references before any provider is destroyed.
  SessionEngine engine;

  InputRouteStatus input_status() const {
    const PassiveInputSource *xi2{};
#ifdef LAYERC_HAVE_X11
    if (x11_connected) xi2 = const_cast<X11Provider &>(x11).input();
#endif
    return input_route_status(evdev, xi2, selected() == runtime::DisplaySession::wayland,
                              engine.input_needs());
  }

  RouteFacts route_facts(const LinuxStatusSnapshot &status) const {
    RouteFacts facts;
    facts.session = selected();
    facts.override_backend = display.override_backend;
#ifdef LAYERC_HAVE_X11
    facts.x11_built = true;
#endif
    facts.x11_connected = x11_connected;
    facts.ewmh = status.x11.ewmh;
    facts.randr = status.x11.randr.available;
    facts.input_selection = input.selection;
    facts.input_route = input_status();
    if (facts.session == runtime::DisplaySession::wayland) {
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
      facts.wayland_present = native != nullptr;
      if (native) {
        facts.toplevels = native->has_windows();
        facts.outputs = native->has_monitors();
        facts.layer_shell = native->overlay() != nullptr;
      }
#endif
#ifdef LAYERC_HAVE_BUS_PROVIDERS
      facts.bus_present = bus != nullptr;
      if (bus) {
        facts.reports = bus->has_windows();
        const auto compositor = detect_compositor(display.desktop,
            bus->kwin_owner(), bus->gnome_owner(), false, false);
        if (compositor == runtime::DisplayCompositor::kwin)
          facts.bus_windows_row = ProviderId::kwin_dbus;
        if (compositor == runtime::DisplayCompositor::gnome)
          facts.bus_windows_row = ProviderId::gnome_shell;
        facts.gnome_overlay_row = compositor == runtime::DisplayCompositor::gnome;
        facts.gnome_overlay = bus->overlay() != nullptr;
      }
#endif
    }
    return facts;
  }
  ProviderSet providers() {
    ProviderSet result;
#ifdef LAYERC_HAVE_X11
    result.x11 = &x11;
    result.x11_connected = x11_connected;
#endif
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
    result.wayland = native.get();
#endif
#ifdef LAYERC_HAVE_BUS_PROVIDERS
    result.bus = bus.get();
#endif
    result.evdev = evdev.enabled() ? &evdev : nullptr;
    return result;
  }
  bool dispatch_bus(std::span<const int> ready) {
#ifdef LAYERC_HAVE_BUS_PROVIDERS
    if (bus) {
      if (selected() != runtime::DisplaySession::wayland)
        bus->configure(false, display.desktop, false);
      return bus->dispatch(ready);
    }
#else
    (void)ready;
#endif
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
  DisplayDispatch dispatch_input(std::span<const int> ready, bool pending) {
    const bool enabled = selected() != runtime::DisplaySession::none;
    if (!enabled) engine.stop_sessions(layerc::core::MonotonicClock::now());
    auto result = engine.poll_input(ready);
#ifdef LAYERC_HAVE_X11
    auto poll = pending ? x11.dispatch_pending() : x11.dispatch(ready);
    result.events += poll.events.size();
    if (enabled) {
      engine.feed_input(poll.events, result);
      result.windows_changed = result.windows_changed || poll.windows_changed;
      result.devices_changed = result.devices_changed || poll.devices_changed;
      result.repaint_requested = result.repaint_requested || poll.repaint_requested;
    }
#else
    (void)pending;
#endif
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
      if (native->take_keymap_changed()) evdev.set_keymap(native->keymap());
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
  enum class Slot { windows, monitors, input, overlay, capture };
  runtime::DisplayProviderStatus health(const ProviderId id, const Slot slot,
      const bool serving, const LinuxStatusSnapshot &status, const RouteFacts &facts) const {
    const auto name = std::string{provider_name(id)};
    switch (id) {
    case ProviderId::x11_ewmh: case ProviderId::x11_randr:
      return {name, State::active, {}};
    case ProviderId::x11_xi2:
      return provider_from_capability(status, runtime::Capability::input, name.c_str(), nullptr);
    case ProviderId::x11_shape:
      return provider_from_capability(status, runtime::Capability::overlay, name.c_str(), nullptr);
    case ProviderId::x11_composite:
      return provider_from_capability(status, runtime::Capability::visual, name.c_str(), nullptr);
    case ProviderId::evdev:
      return evdev_provider(facts.input_route, facts.input_selection);
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
    case ProviderId::wlr_foreign_toplevel:
      if (native) return native->window_provider();
      break;
    case ProviderId::wayland_output:
      if (native) return native->monitor_provider();
      break;
    case ProviderId::wlr_layer_shell:
      if (serving)
        return provider_from_capability(status, runtime::Capability::overlay, name.c_str(), nullptr);
      if (native) return native->overlay_provider();
      break;
#endif
#ifdef LAYERC_HAVE_BUS_PROVIDERS
    case ProviderId::kwin_dbus: case ProviderId::gnome_shell:
      if (bus) {
        const auto row = slot == Slot::overlay ? bus->overlay_provider() : bus->window_provider();
        if (row) return *row;
      }
      break;
    case ProviderId::portal_screencast:
      if (bus) return bus->capture_provider();
      break;
#endif
    default: break;
    }
    (void)slot;
    (void)serving;
    return {name, State::not_built, {"native_provider_not_built"}};
  }
  runtime::DisplayProviderStatus publish_slot(const RouteSlot &plan, const Slot slot,
      const LinuxStatusSnapshot &status, const RouteFacts &facts) const {
    auto row = health(plan.row, slot, plan.serving == plan.row, status, facts);
    row.name = provider_name(plan.row);
    row.reasons.insert(row.reasons.end(), plan.reasons.begin(), plan.reasons.end());
    if (!plan.reasons.empty() && row.state == State::active) row.state = State::degraded;
    for (const auto candidate : plan.shadowed) {
      const auto unavailable = health(candidate, slot, false, status, facts);
      if (unavailable.state == State::unavailable)
        row.reasons.insert(row.reasons.end(), unavailable.reasons.begin(), unavailable.reasons.end());
    }
    return row;
  }
  void publish(LinuxStatusSnapshot &status) const {
    auto value = display;
#ifdef LAYERC_HAVE_WAYLAND_CONNECTION
    value.wayland = wayland.status();
#endif
    const auto facts = route_facts(status);
    const auto plan = resolve_display_route(facts);
    value.session = facts.session;
    if (value.session == runtime::DisplaySession::wayland)
      value.compositor = detect_compositor(value.desktop,
#ifdef LAYERC_HAVE_BUS_PROVIDERS
          bus && bus->kwin_owner(), bus && bus->gnome_owner(),
#else
          false, false,
#endif
          value.wayland.globals.contains("zwlr_layer_shell_v1"),
          value.wayland.globals.contains("zwlr_foreign_toplevel_manager_v1"));
    else value.compositor = runtime::DisplayCompositor::unknown;

    if (plan.session_none != SessionNoneRows::not_applicable) {
      const auto missing = [](ProviderId id, const char *reason) {
        return runtime::DisplayProviderStatus{std::string{provider_name(id)}, State::not_built, {reason}};
      };
      if (plan.session_none == SessionNoneRows::forced_x11_not_built) {
        value.providers = {missing(plan.windows.row, "x11_not_built"),
            missing(plan.monitors.row, "x11_not_built"), missing(plan.input.row, "x11_not_built"),
            missing(plan.overlay.row, "x11_not_built"), missing(plan.capture.row, "x11_not_built")};
      } else if (plan.session_none == SessionNoneRows::forced_x11_unavailable) {
        const runtime::DisplayProviderStatus absent{"none", State::unavailable, {status.x11.reason}};
        value.providers = {absent, absent, absent, absent, absent};
      } else {
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
        const auto lost = [&](ProviderId id) {
          return runtime::DisplayProviderStatus{std::string{provider_name(id)}, State::unavailable,
              {value.wayland.reason.empty() ? std::string{"session_unavailable"} : value.wayland.reason}};
        };
        value.providers.windows = lost(plan.windows.row);
        value.providers.monitors = lost(plan.monitors.row);
        value.providers.overlay = lost(plan.overlay.row);
#else
        value.providers.windows = missing(plan.windows.row, "native_provider_not_built");
        value.providers.monitors = missing(plan.monitors.row, "native_provider_not_built");
        value.providers.overlay = missing(plan.overlay.row, "native_provider_not_built");
#endif
        if (facts.input_route.built && facts.input_selection == InputSelection::evdev &&
            !facts.input_route.evdev_attached)
          value.providers.input = {"evdev", State::unavailable, {"session_unavailable"}};
        else value.providers.input = evdev_provider(facts.input_route, facts.input_selection);
        value.providers.capture = missing(plan.capture.row, "native_provider_not_built");
      }
    } else {
      value.providers.windows = publish_slot(plan.windows, Slot::windows, status, facts);
      value.providers.monitors = publish_slot(plan.monitors, Slot::monitors, status, facts);
      value.providers.input = publish_slot(plan.input, Slot::input, status, facts);
      value.providers.overlay = publish_slot(plan.overlay, Slot::overlay, status, facts);
      value.providers.capture = publish_slot(plan.capture, Slot::capture, status, facts);
    }
    for (const auto &degradation : plan.degradations)
      degrade_capability(status, degradation.capability, degradation.reason.c_str());
    value.reasons.insert(value.reasons.end(), plan.display_reasons.begin(), plan.display_reasons.end());
    status.display = std::move(value);
  }
};

DisplayAdapters::DisplayAdapters() : impl_(std::make_unique<Impl>()) {}
DisplayAdapters::~DisplayAdapters() = default;
layerc::runtime::RuntimeServiceRegistry DisplayAdapters::runtime_services() noexcept {
  return impl_->engine.runtime_services();
}
void DisplayAdapters::start(layerc::platform::EventLoop &loop, LinuxStatusSnapshot &status,
                            std::vector<std::string> &diagnostics) {
  impl_->loop = &loop;
  impl_->evdev.attach(loop);
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
#ifdef LAYERC_HAVE_X11
    impl_->x11.start(loop, status, diagnostics);
    impl_->x11_connected = status.x11.connected;
#else
    (void)diagnostics;
#endif
  } else {
    status.x11.reason = "backend_not_selected";
  }
#ifdef LAYERC_HAVE_BUS_PROVIDERS
  if (impl_->selected() == runtime::DisplaySession::wayland) {
    impl_->bus = std::make_unique<BusProviders>(loop);
  }
#endif
  if (impl_->input.selection == InputSelection::invalid)
    display.reasons.emplace_back("invalid_input_backend");
#ifdef LAYERC_HAVE_WAYLAND_PROVIDERS
  if (impl_->native && impl_->selected() == runtime::DisplaySession::wayland) {
    (void)impl_->native->take_keymap_changed();
    impl_->evdev.set_keymap(impl_->native->keymap());
  }
#endif
  impl_->publish(status);
}
void DisplayAdapters::configure_input(const InputOptions &options) {
  impl_->input = options;
  // A new opt-in applies from the next demand-driven reconcile.
  impl_->engine.release_input();
  impl_->evdev.configure(options);
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
  const auto facts = impl_->route_facts(status);
  const auto plan = resolve_display_route(facts);
  const auto route = bind_display_route(plan, impl_->providers());
#ifdef LAYERC_HAVE_BUS_PROVIDERS
  if (impl_->bus) impl_->bus->use_window_source(route.windows);
#endif
  if (facts.session == runtime::DisplaySession::none) {
    impl_->engine.stop_sessions(layerc::core::MonotonicClock::now());
    impl_->engine.refresh({}, true, route, kernel, status, diagnostics);
  } else {
    impl_->engine.refresh(profiles, configuration_changed, route, kernel, status, diagnostics);
  }
#ifdef LAYERC_HAVE_BUS_PROVIDERS
  if (impl_->bus) impl_->bus->want_overlay(kernel.demand().required(runtime::Capability::overlay));
#endif
  impl_->publish(status);
}
void DisplayAdapters::refresh_status(const layerc::runtime::DaemonKernel &kernel,
                                     LinuxStatusSnapshot &status) const {
  impl_->engine.refresh_status(kernel, status);
  impl_->publish(status);
}
DisplayDispatch DisplayAdapters::dispatch(std::span<const int> ready) {
  const bool wayland_changed = impl_->dispatch_wayland();
  const bool changed = impl_->dispatch_bus(ready) || wayland_changed;
  auto result = impl_->dispatch_input(ready, false);
  result.windows_changed = result.windows_changed || changed;
  return result;
}
bool DisplayAdapters::has_pending_events() {
  bool pending = false;
#ifdef LAYERC_HAVE_X11
  pending = impl_->x11.has_pending_events();
#endif
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
  auto result = impl_->dispatch_input({}, true);
  result.windows_changed = result.windows_changed || changed;
  return result;
}
bool DisplayAdapters::render(layerc::core::TimePoint now) {
  const bool changed = impl_->selected() != runtime::DisplaySession::none && impl_->engine.render(now);
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
  impl_->engine.stop_sessions(now);
#ifdef LAYERC_HAVE_BUS_PROVIDERS
  if (impl_->bus) impl_->bus->stop();
#endif
}
std::optional<layerc::core::TimePoint> DisplayAdapters::next_deadline(layerc::core::TimePoint now) const {
  auto next = impl_->engine.next_deadline(now);
#ifdef LAYERC_HAVE_BUS_PROVIDERS
  if (impl_->bus) next = layerc::core::earliest_deadline(next, impl_->bus->next_deadline());
#endif
  return next;
}
std::size_t DisplayAdapters::session_count() const noexcept { return impl_->engine.session_count(); }
std::vector<layerc::runtime::SessionDiagnostics> DisplayAdapters::session_diagnostics() const {
  return impl_->engine.session_diagnostics();
}
TraceControlResult DisplayAdapters::trace_request(const layerc::core::TraceControlRequest &request) {
  return impl_->engine.trace_request(request);
}
} // namespace layerc::daemon
