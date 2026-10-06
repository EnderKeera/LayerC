#include "layerc/runtime/diagnostics.hpp"
#include "layerc/core/profile_wire.hpp"

#include <chrono>
#include <utility>

namespace layerc::runtime {
namespace {
namespace json = core::json;
using json::Value;

using json::exact_unsigned_value;
Value timestamp(const std::optional<core::TimePoint> time) {
  if (!time) return Value(nullptr);
  const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         time->time_since_epoch()).count();
  return exact_unsigned_value(ticks < 0 ? 0U : static_cast<std::uint64_t>(ticks));
}
Value resources(const RuntimeResourceStats &value) {
  return Value(Value::Object{
      {"threads", exact_unsigned_value(value.threads)}, {"timers", exact_unsigned_value(value.timers)},
      {"handles", exact_unsigned_value(value.handles)}, {"windows", exact_unsigned_value(value.windows)},
      {"objects", exact_unsigned_value(value.objects)}, {"captureSessions", exact_unsigned_value(value.capture_sessions)},
      {"queues", exact_unsigned_value(value.queues)}, {"decodedAssets", exact_unsigned_value(value.decoded_assets)},
      {"cacheBytes", exact_unsigned_value(value.cache_bytes)}});
}
Value process(const ProcessDiagnostics &value) {
  return Value(Value::Object{
      {"pid", exact_unsigned_value(value.pid)}, {"privateBytes", exact_unsigned_value(value.private_bytes)},
      {"workingSetBytes", exact_unsigned_value(value.working_set_bytes)},
      {"handles", exact_unsigned_value(value.handles)}, {"threads", exact_unsigned_value(value.threads)},
      {"kernelTime100ns", exact_unsigned_value(value.kernel_time_100ns)},
      {"userTime100ns", exact_unsigned_value(value.user_time_100ns)}});
}
Value service(const RuntimeServiceStats &value) {
  return Value(Value::Object{
      {"capability", Value(std::string(capability_name(value.capability)))},
      {"state", Value(std::string(service_state_name(value.state)))},
      {"consumers", exact_unsigned_value(value.consumers)}, {"resources", resources(value.resources)},
      {"activationCount", exact_unsigned_value(value.activation_count)}, {"faultCount", exact_unsigned_value(value.fault_count)},
      {"lastActivationNs", timestamp(value.last_activation)},
      {"lastQuiescenceNs", timestamp(value.last_quiescence)}, {"lastError", Value(value.last_error)}});
}
Value session(const SessionDiagnostics &value) {
  return Value(Value::Object{
      {"profileId", Value(value.profile_id)}, {"sourceKey", Value(value.source_key)},
      {"window", json::decimal_unsigned_value(value.window)}, {"foreground", Value(value.foreground)},
      {"overlayWindows", exact_unsigned_value(value.overlay_windows)},
      {"lastInputMatch", Value(value.last_input_match)}});
}
Value helper(const HelperDiagnostics &value) {
  return Value(Value::Object{
      {"window", json::decimal_unsigned_value(value.window)}, {"pid", exact_unsigned_value(value.pid)},
      {"running", Value(value.running)}, {"quarantined", Value(value.quarantined)},
      {"faultCount", exact_unsigned_value(value.fault_count)}, {"lastError", Value(value.last_error)}});
}
Value capture(const CaptureDiagnostics &value) {
  return Value(Value::Object{
      {"window", json::decimal_unsigned_value(value.window)},
      {"runtime", Value(std::string(core::enum_wire_name(core::profile_wire::visual_runtime, value.runtime).value_or("inline")))},
      {"availability", Value(std::string(core::enum_wire_name(
          core::capture_availability_wire, value.health.availability).value_or("inactive")))},
      {"backend", Value(std::string(core::enum_wire_name(
          core::capture_backend_wire, value.health.backend).value_or("none")))},
      {"diagnostic", Value(value.health.diagnostic)}});
}
template <typename T, typename Encode>
Value array(const std::vector<T> &values, Encode encode) {
  Value::Array result;
  result.reserve(values.size());
  for (const auto &value : values) result.push_back(encode(value));
  return Value(std::move(result));
}
Value strings(const std::vector<std::string> &values) {
  return array(values, [](const std::string &value) { return Value(value); });
}
std::string probe_reason(const std::optional<std::string> &reason,
                         const std::string &connection_reason) {
  if (reason) return *reason;
  return connection_reason.empty() ? "extension_missing" : connection_reason;
}
Value extension(const X11ExtensionStatus &value,
                const std::string &connection_reason) {
  Value::Object result{{"available", Value(value.available)}};
  if (value.available) {
    result.emplace("major", Value(static_cast<std::int64_t>(value.major)));
    result.emplace("minor", Value(static_cast<std::int64_t>(value.minor)));
  } else {
    result.emplace("reason", Value(probe_reason(value.reason, connection_reason)));
  }
  return Value(std::move(result));
}
Value x11(const X11StatusSnapshot &value) {
  Value::Object result{{"connected", Value(value.connected)}};
  if (!value.reason.empty()) result.emplace("reason", Value(value.reason));
  result.emplace("xi2", extension(value.xi2, value.reason));
  result.emplace("shape", extension(value.shape, value.reason));
  result.emplace("randr", extension(value.randr, value.reason));
  result.emplace("composite", extension(value.composite, value.reason));
  result.emplace("damage", extension(value.damage, value.reason));
  result.emplace("xfixes", extension(value.xfixes, value.reason));
  Value::Object ewmh{{"available", Value(value.ewmh)}};
  if (!value.ewmh)
    ewmh.emplace("reason", Value(probe_reason(value.ewmh_reason, value.reason)));
  result.emplace("ewmh", Value(std::move(ewmh)));
  return Value(std::move(result));
}
Value capability(const CapabilityStatus &value) {
  Value::Object result{
      {"capability", Value(std::string(capability_name(value.capability)))},
      {"availability", Value(std::string(availability_name(value.availability)))},
      {"reasons", strings(value.reasons)}};
  if (!value.diagnostics.empty())
    result.emplace("diagnostics", strings(value.diagnostics));
  return Value(std::move(result));
}
} // namespace

std::string serialize_runtime_diagnostics_json(const RuntimeDiagnostics &value) {
  Value::Object root{
      {"schemaVersion", exact_unsigned_value(value.schema_version)},
      {"packageVersion", Value(value.package_version)},
      {"protocolVersion", exact_unsigned_value(value.protocol_version)},
      {"process", process(value.process)}, {"services", array(value.services, service)},
      {"gameBarTransport", Value(Value::Object{
          {"connected", Value(value.gamebar.connected)}, {"lastError", Value(value.gamebar.last_error)}})},
      {"settings", Value(Value::Object{
          {"captureExclusion", Value(value.settings.capture_exclusion)},
          {"visualRecognition", Value(value.settings.visual_recognition)},
          {"lastError", Value(value.settings.last_error)}})},
      {"aggregateOwnedResources", resources(value.aggregate_resources)},
      {"sessions", array(value.sessions, session)}, {"isolatedHelpers", array(value.helpers, helper)},
      {"captures", array(value.captures, capture)},
      {"automationFaults", strings(value.automation_faults)}};
  if (value.linux_extension) {
    root.emplace("x11", x11(value.linux_extension->x11));
    root.emplace("capabilities", array(value.linux_extension->capabilities, capability));
  }
  std::string output;
  json::append_value(output, Value(std::move(root)));
  return output;
}

} // namespace layerc::runtime
