#include "layerc/runtime/diagnostics.hpp"

#include <chrono>
#include <utility>

namespace layerc::runtime {
namespace {
namespace json = core::json;
using json::Value;

Value counter(const std::uint64_t value) {
  if (value > static_cast<std::uint64_t>(json::max_exact_json_integer))
    return Value(std::to_string(value));
  return Value(static_cast<std::int64_t>(value));
}
Value timestamp(const std::optional<core::TimePoint> time) {
  if (!time) return Value(nullptr);
  const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         time->time_since_epoch()).count();
  return counter(ticks < 0 ? 0U : static_cast<std::uint64_t>(ticks));
}
Value resources(const RuntimeResourceStats &value) {
  return Value(Value::Object{
      {"threads", counter(value.threads)}, {"timers", counter(value.timers)},
      {"handles", counter(value.handles)}, {"windows", counter(value.windows)},
      {"objects", counter(value.objects)}, {"captureSessions", counter(value.capture_sessions)},
      {"queues", counter(value.queues)}, {"decodedAssets", counter(value.decoded_assets)},
      {"cacheBytes", counter(value.cache_bytes)}});
}
Value process(const ProcessDiagnostics &value) {
  return Value(Value::Object{
      {"pid", counter(value.pid)}, {"privateBytes", counter(value.private_bytes)},
      {"workingSetBytes", counter(value.working_set_bytes)},
      {"handles", counter(value.handles)}, {"threads", counter(value.threads)},
      {"kernelTime100ns", counter(value.kernel_time_100ns)},
      {"userTime100ns", counter(value.user_time_100ns)}});
}
Value service(const RuntimeServiceStats &value) {
  return Value(Value::Object{
      {"capability", Value(std::string(capability_name(value.capability)))},
      {"state", Value(std::string(service_state_name(value.state)))},
      {"consumers", counter(value.consumers)}, {"resources", resources(value.resources)},
      {"activationCount", counter(value.activation_count)}, {"faultCount", counter(value.fault_count)},
      {"lastActivationNs", timestamp(value.last_activation)},
      {"lastQuiescenceNs", timestamp(value.last_quiescence)}, {"lastError", Value(value.last_error)}});
}
Value session(const SessionDiagnostics &value) {
  return Value(Value::Object{
      {"profileId", Value(value.profile_id)}, {"sourceKey", Value(value.source_key)},
      {"window", Value(std::to_string(value.window))}, {"foreground", Value(value.foreground)},
      {"overlayWindows", counter(value.overlay_windows)},
      {"lastInputMatch", Value(value.last_input_match)}});
}
Value helper(const HelperDiagnostics &value) {
  return Value(Value::Object{
      {"window", Value(std::to_string(value.window))}, {"pid", counter(value.pid)},
      {"running", Value(value.running)}, {"quarantined", Value(value.quarantined)},
      {"faultCount", counter(value.fault_count)}, {"lastError", Value(value.last_error)}});
}
template <typename T, typename Encode>
Value array(const std::vector<T> &values, Encode encode) {
  Value::Array result;
  result.reserve(values.size());
  for (const auto &value : values) result.push_back(encode(value));
  return Value(std::move(result));
}
} // namespace

std::string_view runtime_package_version() noexcept { return LAYERC_VERSION; }

std::string serialize_runtime_diagnostics_json(const RuntimeDiagnostics &value) {
  const Value root(Value::Object{
      {"schemaVersion", counter(value.schema_version)},
      {"packageVersion", Value(value.package_version)},
      {"protocolVersion", counter(value.protocol_version)},
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
