#include "x11_adapters.hpp"

#ifndef LAYERC_NO_X11
#include "input_service.hpp"
#include "overlay_service.hpp"
#include "visual_service.hpp"
#include "x11_display.hpp"
#include "x11_monitor.hpp"
#include "x11_window.hpp"
#endif

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace layerc::daemon {

#ifndef LAYERC_NO_X11

namespace {

// Native providers report neutral scans; the session engine still speaks the
// X11 shapes, which carry the same fields.
layerc::platform::X11WindowScan to_x11(const layerc::platform::WindowScan &scan) {
  layerc::platform::X11WindowScan result;
  result.ewmh = scan.authoritative;
  result.active = scan.active;
  result.warning = scan.warning;
  result.truncated = scan.truncated;
  result.windows.reserve(scan.windows.size());
  for (const auto &window : scan.windows)
    result.windows.push_back({window.window, window.title, window.class_name,
                              window.instance_name, window.pid, window.executable_name,
                              window.executable_path, window.x, window.y, window.width,
                              window.height, window.active, window.fullscreen,
                              window.hidden});
  return result;
}

layerc::platform::X11MonitorScan to_x11(const layerc::platform::MonitorScan &scan) {
  layerc::platform::X11MonitorScan result;
  result.randr = scan.available;
  result.warning = scan.warning;
  result.monitors.reserve(scan.monitors.size());
  for (const auto &monitor : scan.monitors)
    result.monitors.push_back({monitor.name, monitor.x, monitor.y, monitor.width,
                               monitor.height, monitor.primary, monitor.dpi});
  return result;
}

} // namespace

struct X11Adapters::Impl {
  std::optional<layerc::platform::X11Display> display;
  std::optional<layerc::platform::X11WindowTracker> tracker;
  X11SessionRegistry sessions;
  X11InputService input{sessions};
  X11AutomationService automation{sessions};
  X11OverlayService overlay;
  X11VisualService visual;
  int fd{-1};
  NativeDisplaySource *native{};

  // XWayland EWMH windows carry PIDs for executable matching; foreign
  // toplevels (no PID) are used only when there is no X11 connection.
  [[nodiscard]] bool native_windows() const {
    return native && native->has_windows() &&
           (native->prefer_windows() || !tracker.has_value());
  }
  [[nodiscard]] bool native_overlay() { return native && native->overlay() != nullptr; }
};

X11Adapters::X11Adapters() : impl_(std::make_unique<Impl>()) {}
X11Adapters::~X11Adapters() = default;

layerc::runtime::RuntimeServiceRegistry
X11Adapters::runtime_services() noexcept {
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

void X11Adapters::set_native(NativeDisplaySource *source) noexcept {
  if (impl_->native != source) impl_->visual.use_native(nullptr);
  impl_->native = source;
  impl_->overlay.use_backend(source ? source->overlay() : nullptr);
}

void X11Adapters::configure_input(const InputOptions &options) {
  impl_->input.configure_input(options);
}
void X11Adapters::prefer_evdev(const bool preferred) noexcept {
  impl_->input.prefer_evdev(preferred);
}
void X11Adapters::set_input_keymap(const std::optional<std::string> &text) {
  impl_->input.set_evdev_keymap(text);
}
InputRouteStatus X11Adapters::input_route() const { return impl_->input.route_status(); }

void X11Adapters::start(layerc::platform::EventLoop &loop,
                        LinuxStatusSnapshot &status,
                        std::vector<std::string> &diagnostics) {
  // evdev needs no X connection (a Wayland session without XWayland).
  impl_->input.attach_loop(loop);
  auto opened = layerc::platform::X11Display::open_result();
  if (!opened.display.has_value()) {
    const char *session_type = std::getenv("XDG_SESSION_TYPE");
    const bool wayland = session_type != nullptr &&
                         std::string_view{session_type} == "wayland";
    // Typed open failure: the reason switches on the error code, never on
    // the human-readable message.
    const char *reason = "session_not_x11";
    if (opened.error ==
        layerc::platform::X11OpenError::cannot_open_display)
      reason = "permission_denied";
    else if (opened.error ==
                 layerc::platform::X11OpenError::no_display_variable &&
             std::getenv("FLATPAK_ID") != nullptr && !wayland)
      reason = "sandbox_denied";
    status.x11.reason = reason;
    for (auto &capability : status.capabilities)
      capability.reasons = {reason};
    diagnostics.push_back(
        opened.message +
        " (input, overlay, automation and capture capabilities stay unavailable)");
    return;
  }

  impl_->display = std::move(*opened.display);
  const auto &probe = impl_->display->probe();
  status.x11 = {
      true,
      {probe.xi2.present, probe.xi2.major, probe.xi2.minor},
      {probe.shape.present, probe.shape.major, probe.shape.minor},
      {probe.randr.present, probe.randr.major, probe.randr.minor},
      {probe.composite.present, probe.composite.major, probe.composite.minor},
      {probe.damage.present, probe.damage.major, probe.damage.minor},
      {probe.xfixes.present, probe.xfixes.major, probe.xfixes.minor},
      probe.ewmh,
      {},
  };
  impl_->fd = impl_->display->fd();
  loop.register_fd(impl_->fd);
  impl_->input.attach(*impl_->display);
  impl_->overlay.attach(*impl_->display);
  impl_->visual.attach(*impl_->display);
  impl_->tracker.emplace(*impl_->display);
  for (const auto &reason : probe.unavailable())
    diagnostics.push_back("x11 " + reason);
}

void X11Adapters::refresh(
    const std::span<const layerc::platform::StoredProfile> profiles,
    const bool configuration_changed,
    layerc::runtime::DaemonKernel &kernel, LinuxStatusSnapshot &status,
    std::vector<std::string> &diagnostics) {
  if (configuration_changed) impl_->sessions.configure(profiles);
  auto *native = impl_->native;
  // Re-evaluated every pass: a provider can appear or vanish at runtime.
  impl_->overlay.use_backend(native ? native->overlay() : nullptr);
  const bool native_windows = impl_->native_windows();
  impl_->visual.use_native(native_windows ? native->capture() : nullptr);
  if (!native_windows && !impl_->tracker.has_value()) {
    (void)kernel.reconcile({});
    refresh_status(kernel, status);
    return;
  }

  const auto scan = native_windows ? to_x11(native->windows()) : impl_->tracker->scan();
  impl_->sessions.refresh(scan);
  const auto monitors =
      native && native->has_monitors() ? to_x11(native->monitors())
      : impl_->display ? layerc::platform::scan_monitors(*impl_->display)
                       : layerc::platform::X11MonitorScan{};
  impl_->overlay.refresh(impl_->sessions.sessions(), scan, monitors);
  const auto requests = impl_->sessions.session_requests();
  const auto demand =
      layerc::runtime::calculate_capability_demand(requests, false);
  // A native factory distinguishes XWayland XIDs from opaque identities.
  impl_->visual.refresh(native_windows && !native->capture()
                            ? std::span<InputSession>{} : impl_->sessions.sessions(),
                        demand, configuration_changed,
                        impl_->sessions.visual_configurations());
  (void)kernel.reconcile(demand);

  const auto input_unavailable = impl_->input.unavailable();
  const auto overlay_unavailable = impl_->overlay.unavailable();
  const auto automation_unavailable = impl_->sessions.automation_diagnostics();
  auto visual_unavailable = impl_->visual.unavailable();
  visual_unavailable.insert(visual_unavailable.end(),
                            impl_->sessions.visual_diagnostics().begin(),
                            impl_->sessions.visual_diagnostics().end());
  if (native_windows && !native->capture() && demand.required(layerc::runtime::Capability::visual))
    visual_unavailable.emplace_back("visual: native windows have no X11 window id");
  derive_capability_status(
      status, demand,
      {.input = input_unavailable,
       .overlay = overlay_unavailable,
       .automation = automation_unavailable,
       .visual = visual_unavailable,
       .native_overlay = impl_->native_overlay(),
       .native_input = impl_->input.evdev_selected()});
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
}

void X11Adapters::refresh_status(
    const layerc::runtime::DaemonKernel &kernel,
    LinuxStatusSnapshot &status) const {
  auto visual_unavailable = impl_->visual.unavailable();
  visual_unavailable.insert(visual_unavailable.end(),
                            impl_->sessions.visual_diagnostics().begin(),
                            impl_->sessions.visual_diagnostics().end());
  if (impl_->native_windows() && !impl_->native->capture() &&
      kernel.demand().required(layerc::runtime::Capability::visual))
    visual_unavailable.emplace_back("visual: native windows have no X11 window id");
  derive_capability_status(
      status, kernel.demand(),
      {.input = impl_->input.unavailable(),
       .overlay = impl_->overlay.unavailable(),
       .automation = impl_->sessions.automation_diagnostics(),
       .visual = visual_unavailable,
       .native_overlay = impl_->native_overlay(),
       .native_input = impl_->input.evdev_selected()});
}

X11Dispatch X11Adapters::dispatch(const std::span<const int> ready) {
  auto result = impl_->input.dispatch_evdev(ready);
  if (impl_->fd < 0 || std::ranges::find(ready, impl_->fd) == ready.end())
    return result;
  auto x11 = impl_->input.dispatch(&*impl_->tracker);
  x11.windows_changed =
      x11.windows_changed ||
      (!impl_->input.attached() && impl_->tracker->poll_changes());
  result.events += x11.events;
  result.windows_changed = result.windows_changed || x11.windows_changed;
  result.devices_changed = result.devices_changed || x11.devices_changed;
  result.triggered.insert(result.triggered.end(),
                          std::make_move_iterator(x11.triggered.begin()),
                          std::make_move_iterator(x11.triggered.end()));
  return result;
}

bool X11Adapters::has_pending_events() noexcept {
  try {
    return impl_->display.has_value() &&
           impl_->display->pending_events() > 0;
  } catch (...) {
    return false;
  }
}

X11Dispatch X11Adapters::dispatch_pending() {
  if (!has_pending_events()) return {};
  const int ready = impl_->fd;
  return dispatch({&ready, 1});
}

bool X11Adapters::render(const layerc::core::TimePoint now) {
  (void)impl_->visual.poll(impl_->sessions.sessions(), now);
  const bool demand_changed = impl_->sessions.poll_automation(now);
  impl_->overlay.render(impl_->sessions.sessions(), now);
  return demand_changed;
}

void X11Adapters::stop_sessions(const layerc::core::TimePoint now) {
  impl_->sessions.stop_sessions(now);
}

std::optional<layerc::core::TimePoint>
X11Adapters::next_deadline(const layerc::core::TimePoint now) const {
  auto next = impl_->overlay.next_render_deadline(now);
  next = layerc::core::earliest_deadline(
      next, impl_->sessions.next_automation_deadline(now));
  return layerc::core::earliest_deadline(next,
                                         impl_->visual.next_deadline(now));
}

std::size_t X11Adapters::session_count() const noexcept {
  return impl_->sessions.session_count();
}

std::vector<layerc::runtime::SessionDiagnostics>
X11Adapters::session_diagnostics() const {
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

TraceControlResult X11Adapters::trace_request(const layerc::core::TraceControlRequest &request) {
  return impl_->sessions.trace_request(request);
}

#else

struct X11Adapters::Impl {};

X11Adapters::X11Adapters() : impl_(std::make_unique<Impl>()) {}
X11Adapters::~X11Adapters() = default;

layerc::runtime::RuntimeServiceRegistry
X11Adapters::runtime_services() noexcept {
  return {};
}

void X11Adapters::set_native(NativeDisplaySource *) noexcept {}
void X11Adapters::configure_input(const InputOptions &) {}
void X11Adapters::prefer_evdev(bool) noexcept {}
void X11Adapters::set_input_keymap(const std::optional<std::string> &) {}
// The session engine (and so evdev routing) lives in the X11-enabled build.
InputRouteStatus X11Adapters::input_route() const { return {}; }

void X11Adapters::start(layerc::platform::EventLoop &, LinuxStatusSnapshot &,
                        std::vector<std::string> &) {}

void X11Adapters::refresh(
    std::span<const layerc::platform::StoredProfile>,
    bool,
    layerc::runtime::DaemonKernel &kernel, LinuxStatusSnapshot &,
    std::vector<std::string> &) {
  (void)kernel.reconcile({});
}

void X11Adapters::refresh_status(
    const layerc::runtime::DaemonKernel &, LinuxStatusSnapshot &) const {}

X11Dispatch X11Adapters::dispatch(std::span<const int>) { return {}; }
bool X11Adapters::has_pending_events() noexcept { return false; }
X11Dispatch X11Adapters::dispatch_pending() { return {}; }
bool X11Adapters::render(layerc::core::TimePoint) { return false; }
void X11Adapters::stop_sessions(layerc::core::TimePoint) {}

std::optional<layerc::core::TimePoint>
X11Adapters::next_deadline(layerc::core::TimePoint) const {
  return std::nullopt;
}

std::size_t X11Adapters::session_count() const noexcept { return 0; }

std::vector<layerc::runtime::SessionDiagnostics>
X11Adapters::session_diagnostics() const {
  return {};
}

TraceControlResult X11Adapters::trace_request(const layerc::core::TraceControlRequest &) {
  return std::string("automation trace unavailable: X11 support was not built");
}

#endif

} // namespace layerc::daemon
