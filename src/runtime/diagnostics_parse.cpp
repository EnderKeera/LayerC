#include "layerc/runtime/diagnostics.hpp"
#include "layerc/core/profile_wire.hpp"

#include <array>
#include <chrono>
#include <limits>
#include <utility>

namespace layerc::runtime {
namespace {
namespace json = core::json;
using json::Required;
using json::EmptyString;

template <typename Integer>
bool counter(json::Reader &reader, const json::Value &root,
             const std::string_view key, const std::string &path, Integer &output,
             const json::IntegerEncoding encoding = json::IntegerEncoding::number_or_string) {
  const auto value = reader.unsigned_integer(root, key, path,
      std::numeric_limits<Integer>::max(), Required::yes, 0, encoding);
  if (!value) return false;
  output = static_cast<Integer>(*value);
  return true;
}
bool timestamp(json::Reader &reader, const json::Value &root,
               const std::string_view key, const std::string &path,
               std::optional<core::TimePoint> &output) {
  const auto *value = reader.field(root, key, path, Required::yes);
  if (!value) return false;
  if (value->is_null()) {
    output.reset();
    return true;
  }
  const auto ticks = reader.unsigned_integer(root, key, path, INT64_MAX,
      Required::yes, 0, json::IntegerEncoding::number_or_string);
  if (!ticks) return false;
  output = core::TimePoint(std::chrono::duration_cast<core::TimePoint::duration>(
      std::chrono::nanoseconds(static_cast<std::int64_t>(*ticks))));
  return true;
}
bool resources(json::Reader &r, const json::Value &root, const std::string &path,
               RuntimeResourceStats &value) {
  return r.exact_fields(
             root, {"threads", "timers", "handles", "windows", "objects",
                    "captureSessions", "queues", "decodedAssets", "cacheBytes"},
             path) &&
         counter(r, root, "threads", path, value.threads) &&
         counter(r, root, "timers", path, value.timers) &&
         counter(r, root, "handles", path, value.handles) &&
         counter(r, root, "windows", path, value.windows) &&
         counter(r, root, "objects", path, value.objects) &&
         counter(r, root, "captureSessions", path, value.capture_sessions) &&
         counter(r, root, "queues", path, value.queues) &&
         counter(r, root, "decodedAssets", path, value.decoded_assets) &&
         counter(r, root, "cacheBytes", path, value.cache_bytes);
}
bool process(json::Reader &r, const json::Value &root, const std::string &path,
             ProcessDiagnostics &value) {
  return r.exact_fields(root,
                        {"pid", "privateBytes", "workingSetBytes", "handles",
                         "threads", "kernelTime100ns", "userTime100ns"},
                        path) &&
         counter(r, root, "pid", path, value.pid) &&
         counter(r, root, "privateBytes", path, value.private_bytes) &&
         counter(r, root, "workingSetBytes", path, value.working_set_bytes) &&
         counter(r, root, "handles", path, value.handles) &&
         counter(r, root, "threads", path, value.threads) &&
         counter(r, root, "kernelTime100ns", path, value.kernel_time_100ns) &&
         counter(r, root, "userTime100ns", path, value.user_time_100ns);
}
bool parse_capability(json::Reader &r, const std::string &path,
                      const std::string &name, Capability &output) {
  const auto parsed = core::parse_enum_wire(capability_wire, name);
  if (!parsed.has_value())
    return r.fail(path + ".capability", "unknown runtime capability");
  output = *parsed;
  return true;
}

bool parse_service_state(json::Reader &r, const std::string &path,
                         const std::string &name, ServiceState &output) {
  const auto parsed = core::parse_enum_wire(service_state_wire, name);
  if (!parsed.has_value())
    return r.fail(path + ".state", "unknown runtime service state");
  output = *parsed;
  return true;
}

bool service(json::Reader &r, const json::Value &root, const std::string &path,
             RuntimeServiceStats &value) {
  if (!r.exact_fields(root,
                      {"capability", "state", "consumers", "resources",
                       "activationCount", "faultCount", "lastActivationNs",
                       "lastQuiescenceNs", "lastError"},
                      path))
    return false;
  std::string capability, state;
  if (!r.string(root, "capability", path, capability, Required::yes, EmptyString::reject) ||
      !r.string(root, "state", path, state, Required::yes, EmptyString::reject))
    return false;
  if (!parse_capability(r, path, capability, value.capability) ||
      !parse_service_state(r, path, state, value.state))
    return false;
  const auto *owned = r.object(root, "resources", path, Required::yes);
  return owned && resources(r, *owned, path + ".resources", value.resources) &&
         counter(r, root, "consumers", path, value.consumers) &&
         counter(r, root, "activationCount", path, value.activation_count) &&
         counter(r, root, "faultCount", path, value.fault_count) &&
         timestamp(r, root, "lastActivationNs", path, value.last_activation) &&
         timestamp(r, root, "lastQuiescenceNs", path, value.last_quiescence) &&
         r.string(root, "lastError", path, value.last_error, Required::yes, EmptyString::allow);
}

bool session(json::Reader &r, const json::Value &root, const std::string &path,
             SessionDiagnostics &value) {
  return r.exact_fields(root,
                        {"profileId", "sourceKey", "window", "foreground",
                         "overlayWindows", "lastInputMatch"},
                        path) &&
         r.string(root, "profileId", path, value.profile_id, Required::yes, EmptyString::allow) &&
         r.string(root, "sourceKey", path, value.source_key, Required::yes, EmptyString::allow) &&
         counter(r, root, "window", path, value.window, json::IntegerEncoding::decimal_string) &&
         r.boolean(root, "foreground", path, value.foreground, Required::yes) &&
         counter(r, root, "overlayWindows", path, value.overlay_windows) &&
         r.string(root, "lastInputMatch", path, value.last_input_match, Required::yes, EmptyString::allow);
}
bool helper(json::Reader &r, const json::Value &root, const std::string &path,
            HelperDiagnostics &value) {
  return r.exact_fields(root,
                        {"window", "pid", "running", "quarantined",
                         "faultCount", "lastError"},
                        path) &&
         counter(r, root, "window", path, value.window, json::IntegerEncoding::decimal_string) &&
         counter(r, root, "pid", path, value.pid) &&
         r.boolean(root, "running", path, value.running, Required::yes) &&
         r.boolean(root, "quarantined", path, value.quarantined, Required::yes) &&
         counter(r, root, "faultCount", path, value.fault_count) &&
         r.string(root, "lastError", path, value.last_error, Required::yes, EmptyString::allow);
}
bool capture(json::Reader &r, const json::Value &root, const std::string &path,
             CaptureDiagnostics &value) {
  std::string runtime, availability, backend;
  if (!r.exact_fields(root, {"window", "runtime", "availability", "backend", "diagnostic"}, path) ||
      !counter(r, root, "window", path, value.window, json::IntegerEncoding::decimal_string) ||
      !r.string(root, "runtime", path, runtime, Required::yes, EmptyString::reject) ||
      !r.string(root, "availability", path, availability, Required::yes, EmptyString::reject) ||
      !r.string(root, "backend", path, backend, Required::yes, EmptyString::reject) ||
      !r.string(root, "diagnostic", path, value.health.diagnostic, Required::yes, EmptyString::allow))
    return false;
  const auto parsed_runtime = core::parse_enum_wire(core::profile_wire::visual_runtime, runtime);
  const auto parsed_availability = core::parse_enum_wire(core::capture_availability_wire, availability);
  const auto parsed_backend = core::parse_enum_wire(core::capture_backend_wire, backend);
  if (!parsed_runtime) return r.fail(path + ".runtime", "unknown visual runtime");
  if (!parsed_availability) return r.fail(path + ".availability", "unknown capture availability");
  if (!parsed_backend) return r.fail(path + ".backend", "unknown capture backend");
  value.runtime = *parsed_runtime;
  value.health.availability = *parsed_availability;
  value.health.backend = *parsed_backend;
  return true;
}
template <typename T, typename Read>
bool array(json::Reader &r, const json::Value &root, const std::string_view key,
           std::vector<T> &output, Read read) {
  const auto *values = r.array(root, key, "$", Required::yes);
  if (!values) return false;
  output.reserve(values->size());
  for (std::size_t index{}; index < values->size(); ++index) {
    T value;
    if (!read(r, (*values)[index], "$." + std::string(key) + "[" + std::to_string(index) + "]", value))
      return false;
    output.push_back(std::move(value));
  }
  return true;
}
bool settings(json::Reader &r, const json::Value &root, SettingsDiagnostics &value) {
  // Older schema-1 producers predate the settings diagnostics section.
  const auto *object = r.object(root, "settings", "$", Required::no);
  if (!object) return r.ok();
  return r.exact_fields(*object,
                        {"captureExclusion", "visualRecognition", "lastError"},
                        "$.settings") &&
         r.boolean(*object, "captureExclusion", "$.settings", value.capture_exclusion, Required::yes) &&
         r.boolean(*object, "visualRecognition", "$.settings", value.visual_recognition, Required::yes) &&
         r.string(*object, "lastError", "$.settings", value.last_error, Required::yes, EmptyString::allow);
}
bool extension(json::Reader &r, const json::Value &root,
               const std::string &path, X11ExtensionStatus &value) {
  if (!r.boolean(root, "available", path, value.available, Required::yes))
    return false;
  if (!value.available) {
    if (!r.exact_fields(root, {"available", "reason"}, path)) return false;
    value.reason = r.string(root, "reason", path, Required::yes, EmptyString::allow);
    return value.reason.has_value();
  }
  if (!r.exact_fields(root, {"available", "major", "minor"}, path)) return false;
  const auto major = r.integer(root, "major", path, 0,
                               std::numeric_limits<int>::max(), Required::yes);
  const auto minor = r.integer(root, "minor", path, 0,
                               std::numeric_limits<int>::max(), Required::yes);
  if (!major || !minor) return false;
  value.major = static_cast<int>(*major);
  value.minor = static_cast<int>(*minor);
  return true;
}
bool x11(json::Reader &r, const json::Value &root, X11StatusSnapshot &value) {
  const std::string path = "$.x11";
  if (!r.exact_fields(root,
                      {"connected", "reason", "xi2", "shape", "randr",
                       "composite", "damage", "xfixes", "ewmh"}, path) ||
      !r.boolean(root, "connected", path, value.connected, Required::yes) ||
      !r.string(root, "reason", path, value.reason, Required::no, EmptyString::allow))
    return false;
  const auto probe = [&](const std::string_view name, X11ExtensionStatus &status) {
    const auto *object = r.object(root, name, path, Required::yes);
    return object && extension(r, *object, path + "." + std::string(name), status);
  };
  if (!probe("xi2", value.xi2) || !probe("shape", value.shape) ||
      !probe("randr", value.randr) || !probe("composite", value.composite) ||
      !probe("damage", value.damage) || !probe("xfixes", value.xfixes))
    return false;
  const auto *ewmh = r.object(root, "ewmh", path, Required::yes);
  if (!ewmh || !r.boolean(*ewmh, "available", path + ".ewmh", value.ewmh, Required::yes))
    return false;
  if (value.ewmh) return r.exact_fields(*ewmh, {"available"}, path + ".ewmh");
  if (!r.exact_fields(*ewmh, {"available", "reason"}, path + ".ewmh")) return false;
  value.ewmh_reason = r.string(*ewmh, "reason", path + ".ewmh", Required::yes, EmptyString::allow);
  return value.ewmh_reason.has_value();
}
bool strings(json::Reader &r, const json::Value &root, const std::string_view key,
             const std::string &path, std::vector<std::string> &output,
             const Required required = Required::yes) {
  const auto *values = r.array(root, key, path, required);
  if (!values) return r.ok();
  return r.each(*values, path + "." + std::string(key),
                [&](const json::Value &value, const std::string &item_path) {
    if (!value.is_string()) r.fail(item_path, "expected string");
    else output.push_back(value.as_string());
  });
}
bool capability(json::Reader &r, const json::Value &root, const std::string &path,
                CapabilityStatus &value) {
  if (!r.exact_fields(root, {"capability", "availability", "reasons", "diagnostics"}, path))
    return false;
  std::string name;
  if (!r.string(root, "capability", path, name, Required::yes, EmptyString::reject) ||
      !parse_capability(r, path, name, value.capability))
    return false;
  const auto availability = r.enumeration<CapabilityAvailability>(
      root, "availability", path, capability_availability_wire, Required::yes,
      CapabilityAvailability::unavailable);
  if (!availability) return false;
  value.availability = *availability;
  return strings(r, root, "reasons", path, value.reasons) &&
         strings(r, root, "diagnostics", path, value.diagnostics, Required::no);
}
bool display_provider(json::Reader &r, const json::Value &root,
                      const std::string &path, DisplayProviderStatus &value) {
  if (!r.exact_fields(root, {"name", "state", "reasons"}, path) ||
      !r.string(root, "name", path, value.name, Required::yes, EmptyString::reject))
    return false;
  const auto state = r.enumeration<DisplayProviderState>(
      root, "state", path, display_provider_state_wire, Required::yes,
      DisplayProviderState::unavailable);
  if (!state) return false;
  value.state = *state;
  return strings(r, root, "reasons", path, value.reasons);
}
bool display(json::Reader &r, const json::Value &root, DisplayStatusSnapshot &value) {
  const std::string path = "$.display";
  if (!r.exact_fields(root, {"session", "override", "desktop", "compositor",
                             "wayland", "bus", "providers", "reasons"}, path))
    return false;
  const auto session = r.enumeration<DisplaySession>(
      root, "session", path, display_session_wire, Required::yes, DisplaySession::none);
  const auto backend = r.enumeration<DisplayBackend>(
      root, "override", path, display_backend_wire, Required::yes, DisplayBackend::auto_detect);
  const auto compositor = r.enumeration<DisplayCompositor>(
      root, "compositor", path, display_compositor_wire, Required::yes, DisplayCompositor::unknown);
  if (!session || !backend || !compositor ||
      !r.string(root, "desktop", path, value.desktop, Required::yes, EmptyString::allow))
    return false;
  value.session = *session;
  value.override_backend = *backend;
  value.compositor = *compositor;
  const auto *wayland = r.object(root, "wayland", path, Required::yes);
  const auto *bus = r.object(root, "bus", path, Required::yes);
  const auto *providers = r.object(root, "providers", path, Required::yes);
  if (!wayland || !bus || !providers ||
      !r.exact_fields(*wayland, {"connected", "globals", "reason"}, path + ".wayland") ||
      !r.boolean(*wayland, "connected", path + ".wayland", value.wayland.connected, Required::yes) ||
      !r.string(*wayland, "reason", path + ".wayland", value.wayland.reason, Required::no, EmptyString::allow) ||
      !r.exact_fields(*bus, {"connected"}, path + ".bus") ||
      !r.boolean(*bus, "connected", path + ".bus", value.bus_connected, Required::yes) ||
      !r.exact_fields(*providers, {"windows", "monitors", "input", "overlay", "capture"}, path + ".providers"))
    return false;
  const auto *globals = r.object(*wayland, "globals", path + ".wayland", Required::yes);
  if (!globals) return false;
  if (globals->as_object().size() > 128) return r.fail(path + ".wayland.globals", "too many globals");
  for (const auto &[name, version] : globals->as_object()) {
    (void)version;
    if (name.empty() || name.size() > 128) return r.fail(path + ".wayland.globals", "invalid interface name");
    const auto number = r.integer(*globals, name, path + ".wayland.globals", 1,
                                   std::numeric_limits<std::uint32_t>::max(), Required::yes);
    if (!number) return false;
    value.wayland.globals.emplace(name, static_cast<std::uint32_t>(*number));
  }
  const auto provider = [&](std::string_view name, DisplayProviderStatus &output) {
    const auto *object = r.object(*providers, name, path + ".providers", Required::yes);
    return object && display_provider(r, *object, path + ".providers." + std::string(name), output);
  };
  return provider("windows", value.providers.windows) &&
         provider("monitors", value.providers.monitors) &&
         provider("input", value.providers.input) &&
         provider("overlay", value.providers.overlay) &&
         provider("capture", value.providers.capture) &&
         strings(r, root, "reasons", path, value.reasons);
}
bool linux_extension(json::Reader &r, const json::Value &root,
                     std::optional<LinuxDiagnostics> &value) {
  const auto *probe = r.field(root, "x11", "$", Required::no);
  const auto *capabilities = r.field(root, "capabilities", "$", Required::no);
  const auto *display_value = r.field(root, "display", "$", Required::no);
  if (!probe && !capabilities && !display_value) return r.ok();
  const auto *object = r.object(root, "x11", "$", Required::yes);
  if (!object) return false;
  value.emplace();
  if (display_value) {
    const auto *display_object = r.object(root, "display", "$", Required::yes);
    if (!display_object) return false;
    value->display.emplace();
    if (!display(r, *display_object, *value->display)) return false;
  }
  return x11(r, *object, value->x11) &&
         array(r, root, "capabilities", value->capabilities, capability);
}
} // namespace

RuntimeDiagnosticsResult parse_runtime_diagnostics_json(const std::string_view source) {
  const auto parsed = json::parse_bounded(source, core::max_live_message_bytes);
  const auto *root = std::get_if<json::Value>(&parsed);
  if (!root) return std::get<json::FieldError>(parsed);
  json::Reader r;
  RuntimeDiagnostics result;
  if (!r.exact_fields(
          *root,
          {"schemaVersion", "packageVersion", "protocolVersion", "process",
           "services", "gameBarTransport", "settings",
           "aggregateOwnedResources", "sessions", "isolatedHelpers", "captures",
           "automationFaults", "overlayFaults", "x11", "capabilities",
           "display"},
          "$") ||
      !r.integer(*root, "schemaVersion", "$", runtime_diagnostics_schema,
                  runtime_diagnostics_schema, Required::yes) ||
      !r.integer(*root, "protocolVersion", "$", core::live_protocol_version,
                  core::live_protocol_version, Required::yes) ||
      !r.string(*root, "packageVersion", "$", result.package_version,
                Required::yes, EmptyString::reject))
    return r.error();
  const auto *process_value = r.object(*root, "process", "$", Required::yes);
  const auto *aggregate = r.object(*root, "aggregateOwnedResources", "$", Required::yes);
  const auto *transport = r.object(*root, "gameBarTransport", "$", Required::yes);
  if (!process_value || !aggregate || !transport ||
      !process(r, *process_value, "$.process", result.process) ||
      !resources(r, *aggregate, "$.aggregateOwnedResources", result.aggregate_resources) ||
      !r.exact_fields(*transport, {"connected", "lastError"},
                      "$.gameBarTransport") ||
      !r.boolean(*transport, "connected", "$.gameBarTransport", result.gamebar.connected, Required::yes) ||
      !r.string(*transport, "lastError", "$.gameBarTransport", result.gamebar.last_error,
                 Required::yes, EmptyString::allow) ||
      !settings(r, *root, result.settings) ||
      !linux_extension(r, *root, result.linux_extension) ||
      !array(r, *root, "services", result.services, service) ||
      !array(r, *root, "sessions", result.sessions, session) ||
      !array(r, *root, "isolatedHelpers", result.helpers, helper))
    return r.error();
  // Older schema-1 producers have no per-window capture diagnostics.
  if (r.field(*root, "captures", "$", Required::no) &&
      !array(r, *root, "captures", result.captures, capture))
    return r.error();
  // Older producers predate the shared automation fault list.
  if (r.field(*root, "automationFaults", "$", Required::no) &&
      !array(r, *root, "automationFaults", result.automation_faults,
             [](json::Reader &reader, const json::Value &value,
                const std::string &path, std::string &line) {
               if (!value.is_string())
                 return reader.fail(path, "expected string");
               line = value.as_string();
               return true;
             }))
    return r.error();
  // Older producers predate the shared overlay item diagnostic list.
  if (!strings(r, *root, "overlayFaults", "$", result.overlay_faults,
               Required::no))
    return r.error();
  std::array<bool, capability_count> seen{};
  for (std::size_t index{}; index < result.services.size(); ++index) {
    const auto capability = static_cast<std::size_t>(result.services[index].capability);
    if (seen[capability])
      return json::FieldError{"$.services[" + std::to_string(index) + "].capability",
                              "duplicate runtime capability"};
    seen[capability] = true;
  }
  return result;
}

} // namespace layerc::runtime
