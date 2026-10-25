#include "display_route.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace layerc::daemon;
using Id = ProviderId;
using Session = layerc::runtime::DisplaySession;
using Backend = layerc::runtime::DisplayBackend;
using Capability = layerc::runtime::Capability;

struct Row {
  std::string name;
  RouteFacts facts;
  RoutePlan expected;
};
RouteSlot slot(Id serving, Id row, std::vector<std::string> reasons = {},
               std::vector<Id> shadowed = {}) {
  return {serving, row, std::move(reasons), std::move(shadowed)};
}
RouteSlot serving(Id id, std::vector<std::string> reasons = {},
                  std::vector<Id> shadowed = {}) {
  return slot(id, id, std::move(reasons), std::move(shadowed));
}
RouteFacts facts_for(Session session, bool x11) {
  RouteFacts facts;
  facts.session = session;
  facts.x11_built = true;
  facts.x11_connected = x11;
  facts.ewmh = facts.randr = true;
  facts.input_route.built = true;
  return facts;
}
void expected_flags(RoutePlan &plan, bool wayland, bool bus) {
  plan.prefer_evdev = wayland;
  plan.native_windows = plan.windows.serving == Id::wlr_foreign_toplevel ||
      plan.windows.serving == Id::kwin_dbus || plan.windows.serving == Id::gnome_shell;
  plan.native_overlay = plan.overlay.serving == Id::wlr_layer_shell ||
      plan.overlay.serving == Id::gnome_shell;
  if (wayland && plan.input.serving == Id::x11_xi2)
    plan.degradations.push_back({Capability::input, "xwayland_focus_only"});
  if (wayland && plan.overlay.serving == Id::x11_shape)
    plan.degradations.push_back({Capability::overlay, "xwayland_overlay"});
  plan.capture.row = Id::portal_screencast;
  if (plan.native_windows && bus) plan.capture.serving = Id::portal_screencast;
  if (plan.windows.serving == Id::x11_ewmh) {
    plan.capture = serving(Id::x11_composite,
        wayland ? std::vector<std::string>{"xwayland_windows_only"} : std::vector<std::string>{});
    if (wayland) plan.degradations.push_back({Capability::visual, "xwayland_windows_only"});
  }
}
RouteSlot xi2(bool wayland) {
  return serving(Id::x11_xi2, wayland
      ? std::vector<std::string>{"xwayland_focus_only", "evdev_not_enabled"}
      : std::vector<std::string>{});
}

std::vector<Row> matrix() {
  std::vector<Row> rows;
  for (bool ewmh : {false, true}) for (bool randr : {false, true}) {
    Row row{"x11 probes " + std::to_string(rows.size()), facts_for(Session::x11, true), {}};
    row.facts.ewmh = ewmh;
    row.facts.randr = randr;
    row.expected.windows = serving(Id::x11_ewmh, ewmh ? std::vector<std::string>{}
        : std::vector<std::string>{"ewmh_missing"});
    row.expected.monitors = serving(Id::x11_randr, randr ? std::vector<std::string>{}
        : std::vector<std::string>{"randr_missing"});
    row.expected.input = xi2(false);
    row.expected.overlay = serving(Id::x11_shape);
    expected_flags(row.expected, false, false);
    rows.push_back(std::move(row));
  }
  for (bool x11 : {false, true}) for (unsigned bits = 0; bits < 8; ++bits)
    for (bool ewmh : {false, true}) for (bool randr : {false, true}) {
    Row row{"native/mixed slots " + std::to_string(rows.size()), facts_for(Session::wayland, x11), {}};
    auto &f = row.facts;
    f.wayland_present = true;
    f.ewmh = ewmh;
    f.randr = randr;
    std::vector<std::string> window_reasons, monitor_reasons;
    if (!ewmh) window_reasons.emplace_back("ewmh_missing");
    if (!randr) monitor_reasons.emplace_back("randr_missing");
    window_reasons.emplace_back("xwayland_windows_only");
    monitor_reasons.emplace_back("xwayland_monitors");
    f.toplevels = (bits & 1) != 0;
    f.outputs = (bits & 2) != 0;
    f.layer_shell = (bits & 4) != 0;
    row.expected.windows = x11 ? serving(Id::x11_ewmh, window_reasons)
        : slot(f.toplevels ? Id::wlr_foreign_toplevel : Id::none, Id::wlr_foreign_toplevel);
    row.expected.monitors = f.outputs ? serving(Id::wayland_output)
        : x11 ? serving(Id::x11_randr, monitor_reasons, {Id::wayland_output})
              : slot(Id::none, Id::wayland_output);
    row.expected.overlay = f.layer_shell ? serving(Id::wlr_layer_shell)
        : x11 ? serving(Id::x11_shape, {"xwayland_overlay"}, {Id::wlr_layer_shell})
              : slot(Id::none, Id::wlr_layer_shell);
    row.expected.input = x11 ? xi2(true) : serving(Id::evdev);
    expected_flags(row.expected, true, false);
    rows.push_back(std::move(row));
  }
  for (Id bus_row : {Id::kwin_dbus, Id::gnome_shell})
    for (bool x11 : {false, true}) for (bool reports : {false, true})
      for (bool toplevels : {false, true}) for (bool bridge : {false, true})
        for (bool layer_shell : {false, true}) {
    Row row{"bus reports/bridge " + std::to_string(rows.size()), facts_for(Session::wayland, x11), {}};
    auto &f = row.facts;
    f.wayland_present = f.outputs = true;
    f.toplevels = toplevels;
    f.layer_shell = layer_shell;
    f.bus_present = true;
    f.bus_windows_row = bus_row;
    f.reports = reports;
    f.gnome_overlay_row = bus_row == Id::gnome_shell;
    f.gnome_overlay = f.gnome_overlay_row && bridge;
    row.expected.windows = reports ? serving(bus_row)
        : x11 ? serving(Id::x11_ewmh, {"xwayland_windows_only"}, {bus_row})
        : toplevels ? serving(Id::wlr_foreign_toplevel, {}, {bus_row})
                    : slot(Id::none, bus_row);
    row.expected.monitors = serving(Id::wayland_output);
    row.expected.input = x11 ? xi2(true) : serving(Id::evdev);
    const std::vector<Id> shadowed = f.gnome_overlay_row
        ? std::vector<Id>{Id::wlr_layer_shell, Id::gnome_shell}
        : std::vector<Id>{Id::wlr_layer_shell};
    row.expected.overlay = layer_shell ? serving(Id::wlr_layer_shell)
        : f.gnome_overlay ? serving(Id::gnome_shell, {}, {Id::wlr_layer_shell})
        : x11 ? serving(Id::x11_shape, {"xwayland_overlay"}, shadowed)
              : slot(Id::none, Id::wlr_layer_shell);
    expected_flags(row.expected, true, true);
    rows.push_back(std::move(row));
  }
  for (const auto *state : {"GNOME bridge unavailable", "GNOME bridge idle", "GNOME bridge active"}) {
    Row row{state, facts_for(Session::wayland, true), {}};
    row.facts.bus_present = true;
    row.facts.bus_windows_row = Id::gnome_shell;
    row.facts.gnome_overlay_row = true;
    row.facts.gnome_overlay = std::string_view(state) == "GNOME bridge active";
    row.expected.windows = serving(Id::x11_ewmh, {"xwayland_windows_only"}, {Id::gnome_shell});
    row.expected.monitors = serving(Id::x11_randr, {"xwayland_monitors"});
    row.expected.input = xi2(true);
    row.expected.overlay = row.facts.gnome_overlay ? serving(Id::gnome_shell)
        : serving(Id::x11_shape, {"xwayland_overlay"}, {Id::gnome_shell});
    expected_flags(row.expected, true, true);
    rows.push_back(std::move(row));
  }
  struct InputRow {
    const char *name;
    InputSelection selection;
    bool selected, attached, xi2_attached;
    std::vector<std::string> faults;
    Id with_x11;
  };
  const std::array input_rows{
      InputRow{"automatic", InputSelection::automatic, false, false, false, {}, Id::x11_xi2},
      InputRow{"invalid", InputSelection::invalid, false, false, false, {}, Id::x11_xi2},
      InputRow{"evdev attached", InputSelection::evdev, true, true, false, {}, Id::evdev},
      InputRow{"evdev gaps", InputSelection::evdev, true, true, false, {"keyboard_unavailable"}, Id::evdev},
      InputRow{"evdev failed XI2 attached", InputSelection::evdev, true, false, true, {"permission_denied"}, Id::x11_xi2},
      InputRow{"evdev failed neither attached", InputSelection::evdev, true, false, false, {"permission_denied"}, Id::evdev},
      InputRow{"evdev pending", InputSelection::evdev, true, false, false, {}, Id::evdev},
      InputRow{"evdev not preferred", InputSelection::evdev, false, false, true, {}, Id::x11_xi2}};
  for (Session session : {Session::x11, Session::wayland}) for (bool x11 : {false, true}) {
    if (session == Session::x11 && !x11) continue;
    for (const auto &input : input_rows) {
      Row row{std::string(input.name) + " " + std::to_string(rows.size()), facts_for(session, x11), {}};
      const bool wayland = session == Session::wayland;
      row.facts.input_selection = input.selection;
      row.facts.input_route = {true, input.selection, input.selected, input.attached, input.xi2_attached, input.faults};
      row.expected.windows = x11 ? serving(Id::x11_ewmh, wayland
          ? std::vector<std::string>{"xwayland_windows_only"} : std::vector<std::string>{})
          : slot(Id::none, Id::wlr_foreign_toplevel);
      row.expected.monitors = x11 ? serving(Id::x11_randr, wayland
          ? std::vector<std::string>{"xwayland_monitors"} : std::vector<std::string>{})
          : slot(Id::none, Id::wayland_output);
      row.expected.overlay = x11 ? serving(Id::x11_shape, wayland
          ? std::vector<std::string>{"xwayland_overlay"} : std::vector<std::string>{})
          : slot(Id::none, Id::wlr_layer_shell);
      const Id identity = x11 ? input.with_x11 : Id::evdev;
      row.expected.input = serving(identity);
      if (identity == Id::x11_xi2 && wayland) {
        row.expected.input.reasons = {"xwayland_focus_only"};
        if (input.selection == InputSelection::evdev)
          row.expected.input.reasons.insert(row.expected.input.reasons.end(), input.faults.begin(), input.faults.end());
        else row.expected.input.reasons.emplace_back(input.selection == InputSelection::invalid
            ? "invalid_input_backend" : "evdev_not_enabled");
      }
      expected_flags(row.expected, wayland, false);
      rows.push_back(std::move(row));
    }
  }
  for (Backend backend : {Backend::auto_detect, Backend::x11, Backend::wayland, Backend::invalid})
    for (bool built : {false, true}) {
    Row row{"headless override " + std::to_string(rows.size()), {}, {}};
    row.facts.override_backend = backend;
    row.facts.x11_built = built;
    row.expected.display_reasons = {backend == Backend::invalid ? "invalid_display_backend" : "session_unavailable"};
    if (backend == Backend::x11) {
      row.expected.session_none = built ? SessionNoneRows::forced_x11_unavailable : SessionNoneRows::forced_x11_not_built;
      if (!built) {
        row.expected.windows.row = Id::x11_ewmh;
        row.expected.monitors.row = Id::x11_randr;
        row.expected.input.row = Id::x11_xi2;
        row.expected.overlay.row = Id::x11_shape;
        row.expected.capture.row = Id::x11_composite;
      }
    } else {
      row.expected.session_none = SessionNoneRows::native;
      row.expected.windows.row = Id::wlr_foreign_toplevel;
      row.expected.monitors.row = Id::wayland_output;
      row.expected.input.row = Id::evdev;
      row.expected.overlay.row = Id::wlr_layer_shell;
      row.expected.capture.row = Id::portal_screencast;
    }
    rows.push_back(std::move(row));
  }
  return rows;
}
void route_matrix() {
  const auto rows = matrix();
  for (const auto &row : rows)
    layerc::test::expect(resolve_display_route(row.facts) == row.expected, row.name);
  std::printf("checked %zu complete route plans\n", rows.size());
}
void identity_names() {
  const std::array<std::string_view, 13> names{
      "x11_ewmh", "x11_randr", "x11_xi2", "x11_shape", "x11_composite",
      "wlr_foreign_toplevel", "wayland_output", "wlr_layer_shell", "kwin_dbus",
      "gnome_shell", "portal_screencast", "evdev", "none"};
  for (std::size_t index = 0; index < names.size(); ++index)
    layerc::test::expect(provider_name(static_cast<Id>(index)) == names[index], "provider row names remain exact");
}
} // namespace
LAYERC_TEST_MAIN(LAYERC_TEST_CASE(route_matrix), LAYERC_TEST_CASE(identity_names))
