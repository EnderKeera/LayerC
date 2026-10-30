#include "layerc/runtime/diagnostics.hpp"
#include "test_support.hpp"

#include <string>
#include <variant>

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

void diagnostics_overlay_fault_contracts() {
  runtime::RuntimeDiagnostics snapshot;
  const auto empty_wire = runtime::serialize_runtime_diagnostics_json(snapshot);
  expect(std::string::npos == empty_wire.find("overlayFaults"),
         "an empty overlay fault list keeps the pre-existing snapshot bytes");
  const auto empty_parsed = runtime::parse_runtime_diagnostics_json(empty_wire);
  const auto *empty_value = std::get_if<runtime::RuntimeDiagnostics>(&empty_parsed);
  expect(empty_value != nullptr && empty_value->overlay_faults.empty(),
         "omitting overlayFaults stays accepted and parses empty");

  snapshot.overlay_faults = {
      "profile \"typed\", panel main, item future: unsupported item kind",
      "profile typed, panel main, item gone: missing source \xC3\xA9"};
  const auto wire = runtime::serialize_runtime_diagnostics_json(snapshot);
  const auto parsed = runtime::parse_runtime_diagnostics_json(wire);
  const auto *value = std::get_if<runtime::RuntimeDiagnostics>(&parsed);
  expect(value != nullptr && value->overlay_faults == snapshot.overlay_faults &&
             runtime::serialize_runtime_diagnostics_json(*value) == wire,
         "nonempty overlay fault lines round-trip with quotes and UTF-8");
  const auto parsed_document = [&](json::Value root) {
    std::string text;
    json::append_value(text, std::move(root));
    return runtime::parse_runtime_diagnostics_json(text);
  };
  auto object_value = document(snapshot).as_object();
  object_value["overlayFaults"] = json::Value(std::string("not a list"));
  const auto wrong_shape = parsed_document(json::Value(std::move(object_value)));
  const auto *shape_error = std::get_if<json::FieldError>(&wrong_shape);
  expect(shape_error != nullptr && shape_error->path == "$.overlayFaults",
         "a non-list overlayFaults value is rejected at its path");
  auto element = document(snapshot).as_object();
  element["overlayFaults"] = json::Value(json::Value::Array{json::Value(std::int64_t{7})});
  const auto wrong_element = parsed_document(json::Value(std::move(element)));
  const auto *element_error = std::get_if<json::FieldError>(&wrong_element);
  expect(element_error != nullptr &&
             element_error->path == "$.overlayFaults[0]",
         "a non-string overlay fault line is rejected at its index");
}
