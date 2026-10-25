#include "display_route.hpp"
#include "session_engine.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <utility>

namespace layerc::daemon {

std::vector<PassiveInputSource *> select_input_sources(
    PassiveInputSource *evdev, PassiveInputSource *xi2, const bool prefer_evdev) {
  std::vector<PassiveInputSource *> sources;
  if (evdev && (prefer_evdev || !xi2 || !xi2->input_available())) sources.push_back(evdev);
  if (xi2 && std::ranges::find(sources, xi2) == sources.end()) sources.push_back(xi2);
  return sources;
}

namespace {
using Id = ProviderId;
using Session = layerc::runtime::DisplaySession;
using Backend = layerc::runtime::DisplayBackend;
using Capability = layerc::runtime::Capability;
struct Candidate {
  Id id;
  bool relevant;
  bool available;
};

RouteSlot choose(const std::initializer_list<Candidate> candidates, const Id absent) {
  RouteSlot result;
  result.row = absent;
  std::vector<Id> preceding;
  bool first = true;
  for (const auto &candidate : candidates) {
    if (!candidate.relevant) continue;
    if (first) result.row = candidate.id;
    first = false;
    if (candidate.available) {
      result.serving = result.row = candidate.id;
      result.shadowed = std::move(preceding);
      return result;
    }
    preceding.push_back(candidate.id);
  }
  return result;
}

void x11_reasons(RouteSlot &slot, const Id x11, const bool probe,
                 const char *missing, const bool wayland, const char *fallback) {
  if (slot.serving != x11) return;
  if (!probe) slot.reasons.emplace_back(missing);
  if (wayland) slot.reasons.emplace_back(fallback);
}
} // namespace

std::string_view provider_name(const ProviderId provider) noexcept {
  static constexpr std::array names{
      "x11_ewmh", "x11_randr", "x11_xi2", "x11_shape", "x11_composite",
      "wlr_foreign_toplevel", "wayland_output", "wlr_layer_shell",
      "kwin_dbus", "gnome_shell", "portal_screencast", "evdev", "none"};
  const auto index = static_cast<std::size_t>(provider);
  return index < names.size() ? names[index] : "none";
}

RoutePlan resolve_display_route(const RouteFacts &facts) {
  RoutePlan plan;
  const bool wayland = facts.session == Session::wayland;
  plan.prefer_evdev = wayland;
  if (facts.session == Session::none) {
    plan.display_reasons.emplace_back(facts.override_backend == Backend::invalid
        ? "invalid_display_backend" : "session_unavailable");
    if (facts.override_backend == Backend::x11) {
      plan.session_none = facts.x11_built ? SessionNoneRows::forced_x11_unavailable
                                         : SessionNoneRows::forced_x11_not_built;
      if (!facts.x11_built) {
        plan.windows.row = Id::x11_ewmh;
        plan.monitors.row = Id::x11_randr;
        plan.input.row = Id::x11_xi2;
        plan.overlay.row = Id::x11_shape;
        plan.capture.row = Id::x11_composite;
      }
    } else {
      plan.session_none = SessionNoneRows::native;
      plan.windows.row = Id::wlr_foreign_toplevel;
      plan.monitors.row = Id::wayland_output;
      plan.input.row = Id::evdev;
      plan.overlay.row = Id::wlr_layer_shell;
      plan.capture.row = Id::portal_screencast;
    }
    return plan;
  }
  const bool native = wayland && facts.wayland_present;
  const bool bus = wayland && facts.bus_present;
  plan.windows = choose({
      {facts.bus_windows_row, bus && facts.bus_windows_row != Id::none, facts.reports},
      {Id::x11_ewmh, facts.x11_connected, true},
      {Id::wlr_foreign_toplevel, native, facts.toplevels}}, Id::wlr_foreign_toplevel);
  plan.monitors = choose({{Id::wayland_output, native, facts.outputs},
      {Id::x11_randr, facts.x11_connected, true}}, Id::wayland_output);
  plan.overlay = choose({{Id::wlr_layer_shell, native, facts.layer_shell},
      {Id::gnome_shell, bus && facts.gnome_overlay_row, facts.gnome_overlay},
      {Id::x11_shape, facts.x11_connected, true}}, Id::wlr_layer_shell);
  x11_reasons(plan.windows, Id::x11_ewmh, facts.ewmh, "ewmh_missing",
              wayland, "xwayland_windows_only");
  x11_reasons(plan.monitors, Id::x11_randr, facts.randr, "randr_missing",
              wayland, "xwayland_monitors");
  plan.native_windows = plan.windows.serving == Id::kwin_dbus ||
      plan.windows.serving == Id::gnome_shell ||
      plan.windows.serving == Id::wlr_foreign_toplevel;
  plan.native_overlay = plan.overlay.serving == Id::wlr_layer_shell ||
      plan.overlay.serving == Id::gnome_shell;

  const auto &input = facts.input_route;
  const bool evdev = input.evdev_selected && (input.evdev_attached || !input.xi2_attached);
  plan.input.serving = plan.input.row = evdev || !facts.x11_connected ? Id::evdev : Id::x11_xi2;
  if (plan.input.serving == Id::x11_xi2 && wayland) {
    plan.input.reasons.emplace_back("xwayland_focus_only");
    if (facts.input_selection == InputSelection::evdev)
      plan.input.reasons.insert(plan.input.reasons.end(), input.evdev_reasons.begin(), input.evdev_reasons.end());
    else
      plan.input.reasons.emplace_back(facts.input_selection == InputSelection::invalid
          ? "invalid_input_backend" : "evdev_not_enabled");
    plan.degradations.push_back({Capability::input, "xwayland_focus_only"});
  }
  if (plan.overlay.serving == Id::x11_shape && wayland) {
    plan.overlay.reasons.emplace_back("xwayland_overlay");
    plan.degradations.push_back({Capability::overlay, "xwayland_overlay"});
  }
  plan.capture.row = Id::portal_screencast;
  if (plan.native_windows) {
    if (bus) plan.capture.serving = Id::portal_screencast;
  } else if (plan.windows.serving == Id::x11_ewmh) {
    plan.capture.serving = plan.capture.row = Id::x11_composite;
    if (wayland) {
      plan.capture.reasons.emplace_back("xwayland_windows_only");
      plan.degradations.push_back({Capability::visual, "xwayland_windows_only"});
    }
  }
  return plan;
}

DisplayRoute bind_display_route(const RoutePlan &plan, const ProviderSet &providers) {
  const auto source = [&](const ProviderId id) -> DisplaySource * {
    switch (id) {
    case ProviderId::x11_ewmh: case ProviderId::x11_randr:
    case ProviderId::x11_shape: case ProviderId::x11_composite:
      return providers.x11;
    case ProviderId::wlr_foreign_toplevel: case ProviderId::wayland_output:
    case ProviderId::wlr_layer_shell:
      return providers.wayland;
    case ProviderId::kwin_dbus: case ProviderId::gnome_shell:
    case ProviderId::portal_screencast:
      return providers.bus;
    default: return nullptr;
    }
  };
  DisplayRoute route;
  route.windows = source(plan.windows.serving);
  route.monitors = source(plan.monitors.serving);
  if (auto *provider = source(plan.overlay.serving)) route.overlay = provider->overlay();
  if (plan.native_windows)
    if (auto *provider = source(plan.capture.serving)) route.capture = provider->capture();
  PassiveInputSource *xi2{};
  if (providers.x11_connected && providers.x11) {
    xi2 = providers.x11->input();
    route.x11_capture = providers.x11->capture();
  }
  route.inputs = select_input_sources(providers.evdev, xi2, plan.prefer_evdev);
  route.native_input = !route.inputs.empty() && route.inputs.front() == providers.evdev;
  route.native_windows = plan.native_windows;
  route.native_overlay = plan.native_overlay;
  return route;
}

} // namespace layerc::daemon
