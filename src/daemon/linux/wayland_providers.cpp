#include "wayland_providers.hpp"

#include "wayland_connection.hpp"
#include "wayland_keymap.hpp"
#include "wayland_monitors.hpp"
#include "wayland_overlay.hpp"
#include "wayland_toplevels.hpp"

#include <utility>

namespace layerc::daemon {

namespace runtime = layerc::runtime;
using State = runtime::DisplayProviderState;

struct WaylandProviders::Impl {
  layerc::platform::WaylandConnection &connection;
  layerc::platform::WaylandMonitors monitors;
  layerc::platform::WaylandToplevels toplevels;
  layerc::platform::WaylandOverlay overlay;
  layerc::platform::WaylandKeymap keymap;

  explicit Impl(layerc::platform::WaylandConnection &owner) : connection(owner) {}
  void release() noexcept {
    keymap.release();
    overlay.release();
    toplevels.release();
    monitors.release();
  }
};

WaylandProviders::WaylandProviders(layerc::platform::WaylandConnection &connection)
    : impl_(std::make_unique<Impl>(connection)) {
  sync();
  // Registered last: a throwing constructor must not leave a dangling hook.
  connection.set_release_hook([impl = impl_.get()] { impl->release(); });
}

WaylandProviders::~WaylandProviders() {
  impl_->connection.set_release_hook({});
  impl_->release();
  impl_->connection.flush();
}

void WaylandProviders::sync() {
  impl_->monitors.sync(impl_->connection);
  // Destroyed output proxies must not alias a later output's address.
  impl_->toplevels.forget_outputs(impl_->monitors.take_removed());
  impl_->toplevels.sync(impl_->connection);
  impl_->overlay.sync(impl_->connection);
  impl_->keymap.sync(impl_->connection);
  impl_->connection.flush();
}

bool WaylandProviders::take_keymap_changed() noexcept { return impl_->keymap.take_changed(); }

const std::optional<std::string> &WaylandProviders::keymap() const noexcept {
  return impl_->keymap.text();
}

bool WaylandProviders::take_changed() noexcept {
  // Evaluate every provider so each one's flag is consumed.
  const bool monitors = impl_->monitors.take_changed();
  const bool toplevels = impl_->toplevels.take_changed();
  const bool overlay = impl_->overlay.take_changed();
  return monitors || toplevels || overlay;
}

void WaylandProviders::repaint() {
  impl_->overlay.repaint();
  impl_->connection.flush();
}

bool WaylandProviders::has_windows() const { return impl_->toplevels.available(); }

layerc::platform::WindowScan WaylandProviders::windows() const {
  return impl_->toplevels.scan(impl_->monitors);
}

bool WaylandProviders::has_monitors() const { return impl_->monitors.scan().available; }

layerc::platform::MonitorScan WaylandProviders::monitors() const {
  return impl_->monitors.scan();
}

OverlayBackend *WaylandProviders::overlay() {
  return impl_->overlay.unavailable().empty() ? this : nullptr;
}

bool WaylandProviders::show(const std::string_view key,
                            const layerc::platform::OverlayFrame &frame) {
  const auto list = layerc::platform::build_overlay_display_list(frame);
  const auto &bounds = list.bounds;
  const auto scan = impl_->monitors.scan();
  const auto *monitor = layerc::platform::monitor_for_rect(scan, bounds.x, bounds.y,
                                                           bounds.width, bounds.height);
  if (bounds.width <= 0 || bounds.height <= 0 || !monitor) {
    impl_->overlay.remove(key);
    return false;
  }
  return impl_->overlay.present(key, list, impl_->monitors.output(monitor->name), *monitor);
}

void WaylandProviders::hide(const std::string_view key) noexcept { impl_->overlay.remove(key); }

void WaylandProviders::retain(const std::span<const std::string> keys) noexcept {
  impl_->overlay.retain(keys);
}

std::vector<std::string> WaylandProviders::unavailable() const {
  auto reasons = impl_->overlay.unavailable();
  if (reasons.empty() && !impl_->overlay.fault().empty())
    reasons.push_back(impl_->overlay.fault());
  return reasons;
}

runtime::DisplayProviderStatus WaylandProviders::window_provider() const {
  if (!impl_->toplevels.available())
    return {"wlr_foreign_toplevel", State::unavailable, {"foreign_toplevel_missing"}};
  // The protocol carries no PID (no executable matching) and no geometry
  // (panels anchor to the toplevel's output).
  return {"wlr_foreign_toplevel", State::degraded,
          {"toplevel_no_pid", "toplevel_output_geometry"}};
}

runtime::DisplayProviderStatus WaylandProviders::monitor_provider() const {
  const auto scan = impl_->monitors.scan();
  if (!scan.available) return {"wayland_output", State::unavailable, {scan.warning}};
  if (!scan.warning.empty()) return {"wayland_output", State::degraded, {scan.warning}};
  return {"wayland_output", State::active, {}};
}

runtime::DisplayProviderStatus WaylandProviders::overlay_provider() const {
  auto reasons = unavailable();
  if (!reasons.empty()) return {"wlr_layer_shell", State::unavailable, std::move(reasons)};
  return {"wlr_layer_shell", State::active, {}};
}

} // namespace layerc::daemon
