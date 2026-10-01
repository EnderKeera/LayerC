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
} // namespace

RuntimeDiagnosticsResult parse_runtime_diagnostics_json(const std::string_view source) {
  if (source.size() > core::max_live_message_bytes)
    return json::FieldError{"$", "diagnostics snapshot exceeds the live message limit"};
  const auto parsed = json::parse(source);
  const auto *root = std::get_if<json::Value>(&parsed);
  if (!root) {
    const auto &error = std::get<json::ParseError>(parsed);
    return json::FieldError{"$@" + std::to_string(error.offset), error.message};
  }
  json::Reader r;
  RuntimeDiagnostics result;
  if (!r.exact_fields(
          *root,
          {"schemaVersion", "packageVersion", "protocolVersion", "process",
           "services", "gameBarTransport", "settings",
           "aggregateOwnedResources", "sessions", "isolatedHelpers", "captures"},
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
      !array(r, *root, "services", result.services, service) ||
      !array(r, *root, "sessions", result.sessions, session) ||
      !array(r, *root, "isolatedHelpers", result.helpers, helper))
    return r.error();
  // Older schema-1 producers have no per-window capture diagnostics.
  if (r.field(*root, "captures", "$", Required::no) &&
      !array(r, *root, "captures", result.captures, capture))
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
