#include "layerc/runtime/diagnostics.hpp"

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
      {"window", Value(std::to_string(value.window))}, {"foreground", Value(value.foreground)},
      {"overlayWindows", exact_unsigned_value(value.overlay_windows)},
      {"lastInputMatch", Value(value.last_input_match)}});
}
Value helper(const HelperDiagnostics &value) {
  return Value(Value::Object{
      {"window", Value(std::to_string(value.window))}, {"pid", exact_unsigned_value(value.pid)},
      {"running", Value(value.running)}, {"quarantined", Value(value.quarantined)},
      {"faultCount", exact_unsigned_value(value.fault_count)}, {"lastError", Value(value.last_error)}});
}
template <typename T, typename Encode>
Value array(const std::vector<T> &values, Encode encode) {
  Value::Array result;
  result.reserve(values.size());
  for (const auto &value : values) result.push_back(encode(value));
  return Value(std::move(result));
}
} // namespace

std::string serialize_runtime_diagnostics_json(const RuntimeDiagnostics &value) {
  const Value root(Value::Object{
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
      {"sessions", array(value.sessions, session)}, {"isolatedHelpers", array(value.helpers, helper)}});
  std::string output;
  json::append_value(output, root);
  return output;
}

} // namespace layerc::runtime
