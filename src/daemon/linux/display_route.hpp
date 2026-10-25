#pragma once

#include "display_selection.hpp"
#include "layerc/runtime/runtime_services.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::daemon {

class PassiveInputSource;

// The one authority for passive input preference: an opted-in evdev source
// precedes XI2, and XI2 is the fallback whenever evdev cannot serve.
[[nodiscard]] std::vector<PassiveInputSource *> select_input_sources(
    PassiveInputSource *evdev, PassiveInputSource *xi2, bool prefer_evdev);

enum class ProviderId : std::uint8_t {
  x11_ewmh, x11_randr, x11_xi2, x11_shape, x11_composite,
  wlr_foreign_toplevel, wayland_output, wlr_layer_shell,
  kwin_dbus, gnome_shell, portal_screencast, evdev, none,
};

[[nodiscard]] std::string_view provider_name(ProviderId provider) noexcept;

struct RouteFacts {
  layerc::runtime::DisplaySession session{layerc::runtime::DisplaySession::none};
  layerc::runtime::DisplayBackend override_backend{layerc::runtime::DisplayBackend::auto_detect};
  bool x11_built{}, x11_connected{}, ewmh{}, randr{};
  bool wayland_present{}, toplevels{}, outputs{}, layer_shell{};
  bool bus_present{}, reports{};
  ProviderId bus_windows_row{ProviderId::none};
  bool gnome_overlay{}, gnome_overlay_row{};
  InputSelection input_selection{InputSelection::automatic};
  InputRouteStatus input_route;
};

struct RouteSlot {
  ProviderId serving{ProviderId::none};
  ProviderId row{ProviderId::none};
  std::vector<std::string> reasons;
  // Publication appends only unavailable health rows, never inactive rows.
  std::vector<ProviderId> shadowed;
  bool operator==(const RouteSlot &) const = default;
};

enum class SessionNoneRows : std::uint8_t {
  not_applicable, forced_x11_not_built, forced_x11_unavailable, native,
};

struct RouteDegradation {
  layerc::runtime::Capability capability;
  std::string reason;
  bool operator==(const RouteDegradation &) const = default;
};

struct RoutePlan {
  RouteSlot windows, monitors, input, overlay, capture;
  bool prefer_evdev{}, native_windows{}, native_overlay{};
  SessionNoneRows session_none{SessionNoneRows::not_applicable};
  // Session-none publication retains build/session health and literal rows.
  std::vector<std::string> display_reasons;
  std::vector<RouteDegradation> degradations;
  bool operator==(const RoutePlan &) const = default;
};

[[nodiscard]] RoutePlan resolve_display_route(const RouteFacts &facts);

class DisplaySource;
struct DisplayRoute;
struct ProviderSet {
  DisplaySource *x11{}, *wayland{}, *bus{};
  bool x11_connected{};
  PassiveInputSource *evdev{}; // set only when the user opted in
};

[[nodiscard]] DisplayRoute bind_display_route(const RoutePlan &plan,
                                               const ProviderSet &providers);

} // namespace layerc::daemon
