#include "layerc/runtime/diagnostics.hpp"
#include "test_support.hpp"

namespace {
namespace runtime = layerc::runtime;
namespace json = layerc::core::json;
using layerc::test::expect;
json::Value document(const runtime::RuntimeDiagnostics &value) {
  return std::get<json::Value>(json::parse(runtime::serialize_runtime_diagnostics_json(value)));
}
} // namespace

void diagnostics_display_contracts() {
  runtime::RuntimeDiagnostics snapshot;
  snapshot.linux_extension.emplace();
  snapshot.linux_extension->x11.reason = "session_not_x11";
  const auto original_wire = runtime::serialize_runtime_diagnostics_json(snapshot);
  const auto original = document(snapshot);
  snapshot.linux_extension->display.emplace();
  auto &display = *snapshot.linux_extension->display;
  display.session = runtime::DisplaySession::wayland;
  display.desktop = "KDE:\"é\"";
  display.compositor = runtime::DisplayCompositor::kwin;
  display.wayland.connected = true;
  display.wayland.globals = {{"wl_compositor", 6}, {"zwlr_layer_shell_v1", 4}};
  display.providers.windows = {"kwin_script", runtime::DisplayProviderState::active, {}};
  display.providers.monitors = {"wayland_output", runtime::DisplayProviderState::active, {}};
  display.providers.input = {"x11_xi2", runtime::DisplayProviderState::degraded, {"xwayland_focus_only"}};
  display.providers.overlay = {"wlr_layer_shell", runtime::DisplayProviderState::inactive, {"not_demanded"}};
  display.providers.capture = {"portal_screencast", runtime::DisplayProviderState::not_built, {"native_provider_not_built"}};
  auto current = document(snapshot).as_object();
  std::string x11_old, x11_new;
  json::append_value(x11_old, *original.find("x11"));
  json::append_value(x11_new, current.at("x11"));
  expect(x11_old == x11_new, "display extension leaves X11 object byte-identical");
  // Everything else is unchanged as well; old producers omit display.
  auto without_display = current;
  without_display.erase("display");
  std::string old_wire;
  json::append_value(old_wire, json::Value(std::move(without_display)));
  expect(old_wire == original_wire, "optional display preserves pre-V2.7 snapshot bytes");

  for (const auto &[state, name] : runtime::display_provider_state_wire) {
    (void)name;
    display.providers.capture.state = state;
    const auto wire = runtime::serialize_runtime_diagnostics_json(snapshot);
    const auto parsed = runtime::parse_runtime_diagnostics_json(wire);
    const auto *value = std::get_if<runtime::RuntimeDiagnostics>(&parsed);
    // Parsed X11 extension reasons are explicit on the wire.
    expect(value && value->linux_extension->display == snapshot.linux_extension->display &&
           runtime::serialize_runtime_diagnostics_json(*value) == wire,
           "all provider states round-trip with stable JSON and UTF-8");
  }
  const auto rejects = [&](std::string_view path, auto mutate) {
    auto root = current;
    auto fields = root.at("display").as_object();
    mutate(fields);
    root["display"] = json::Value(std::move(fields));
    std::string wire;
    json::append_value(wire, json::Value(std::move(root)));
    const auto parsed = runtime::parse_runtime_diagnostics_json(wire);
    const auto *error = std::get_if<json::FieldError>(&parsed);
    expect(error && error->path == path, "invalid display field retains exact error path");
  };
  rejects("$.display.session", [](auto &fields) { fields["session"] = json::Value("bogus"); });
  rejects("$.display.override", [](auto &fields) { fields.erase("override"); });
  rejects("$.display.bus", [](auto &fields) { fields["bus"] = json::Value(nullptr); });
  rejects("$.display.providers.input.state", [](auto &fields) {
    auto providers = fields.at("providers").as_object();
    auto input = providers.at("input").as_object();
    input["state"] = json::Value("bogus");
    providers["input"] = json::Value(std::move(input));
    fields["providers"] = json::Value(std::move(providers));
  });
  rejects("$.display.wayland.globals.wl_compositor", [](auto &fields) {
    auto wayland = fields.at("wayland").as_object();
    wayland["globals"] = json::Value(json::Value::Object{{"wl_compositor", json::Value(std::int64_t{-1})}});
    fields["wayland"] = json::Value(std::move(wayland));
  });
  rejects("$.display.wayland.globals", [](auto &fields) {
    auto wayland = fields.at("wayland").as_object();
    json::Value::Object globals;
    for (int i = 0; i < 129; ++i) globals.emplace("global" + std::to_string(i), json::Value(std::int64_t{1}));
    wayland["globals"] = json::Value(std::move(globals));
    fields["wayland"] = json::Value(std::move(wayland));
  });
  auto orphan = current;
  orphan.erase("x11");
  orphan.erase("capabilities");
  std::string wire;
  json::append_value(wire, json::Value(std::move(orphan)));
  const auto parsed = runtime::parse_runtime_diagnostics_json(wire);
  const auto *error = std::get_if<json::FieldError>(&parsed);
  expect(error && error->path == "$.x11", "display without Linux probes is rejected");
}
