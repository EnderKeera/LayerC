#include "layerc/core/automation_trace_control.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/utf8.hpp"

#include <algorithm>
#include <limits>

namespace layerc::core {
namespace {
const json::Value *field(const json::Value &v, std::string_view name) { return v.find(name); }
bool version_ok(const json::Value &v) {
  const auto *version = field(v, "traceVersion");
  return version && version->is_integer() && version->as_integer() == trace_control_version;
}
std::optional<std::uint64_t> number(const json::Value &v, std::string_view name) {
  const auto *value = field(v, name);
  if (!value || !value->is_integer() || value->as_integer() < 0) return std::nullopt;
  return static_cast<std::uint64_t>(value->as_integer());
}
bool unsigned_fits(std::uint64_t value) {
  return value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}
} // namespace

bool trace_control_request_valid(const TraceControlRequest &request) noexcept {
  const bool client_ok = !request.client_id.empty() &&
      request.client_id.size() <= trace_control_client_bytes &&
      std::ranges::all_of(request.client_id, [](const char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
      });
  return client_ok && !request.profile_id.empty() &&
         request.profile_id.size() <= max_automation_string_bytes &&
         is_valid_utf8(request.profile_id) &&
         enum_wire_name(trace_control_operation_wire, request.operation).has_value() &&
         unsigned_fits(request.generation) && unsigned_fits(request.cursor);
}
std::string serialize_trace_control_request(const TraceControlRequest &request) {
  std::string out = "{\"traceVersion\":1,\"operation\":";
  json::append_quoted(out, enum_wire_name(trace_control_operation_wire, request.operation).value_or(""));
  out += ",\"profileId\":"; json::append_quoted(out, request.profile_id);
  out += ",\"clientId\":"; json::append_quoted(out, request.client_id);
  out += ",\"generation\":"; json::append_unsigned(out, request.generation);
  out += ",\"cursor\":"; json::append_unsigned(out, request.cursor);
  out += "}";
  return out;
}
std::optional<TraceControlRequest> parse_trace_control_request(const std::string_view text) {
  if (text.size() > 8U * max_automation_string_bytes) return std::nullopt;
  const auto parsed = json::parse(text);
  const auto *v = std::get_if<json::Value>(&parsed);
  if (!v || !v->is_object() || v->as_object().size() != 6 || !version_ok(*v)) return std::nullopt;
  const auto *op = field(*v, "operation"), *profile = field(*v, "profileId"), *client = field(*v, "clientId");
  const auto generation = number(*v, "generation"), cursor = number(*v, "cursor");
  if (!op || !op->is_string() || !profile || !profile->is_string() ||
      !client || !client->is_string() || !generation || !cursor) return std::nullopt;
  const auto operation = parse_enum_wire(trace_control_operation_wire, op->as_string());
  if (!operation) return std::nullopt;
  TraceControlRequest request{*operation, profile->as_string(), client->as_string(), *generation, *cursor};
  if (!trace_control_request_valid(request)) return std::nullopt;
  return request;
}
std::string serialize_trace_control_reply(const TraceControlReply &reply) {
  std::string out = "{\"traceVersion\":1,\"generation\":";
  json::append_unsigned(out, reply.generation);
  out += ",\"cursor\":"; json::append_unsigned(out, reply.cursor);
  out += ",\"reset\":"; json::append_bool(out, reply.reset);
  out += ",\"gap\":"; json::append_bool(out, reply.gap);
  out += ",\"packet\":"; json::append_quoted(out, reply.packet);
  out += "}";
  return out;
}
std::optional<TraceControlReply> parse_trace_control_reply(const std::string_view text) {
  if (text.size() > 2U * trace_control_packet_bytes + 1024U) return std::nullopt;
  const auto parsed = json::parse(text);
  const auto *v = std::get_if<json::Value>(&parsed);
  if (!v || !v->is_object() || v->as_object().size() != 6 || !version_ok(*v)) return std::nullopt;
  const auto generation = number(*v, "generation"), cursor = number(*v, "cursor");
  const auto *reset = field(*v, "reset"), *gap = field(*v, "gap"), *packet = field(*v, "packet");
  if (!generation || !cursor || !reset || !reset->is_bool() || !gap || !gap->is_bool() ||
      !packet || !packet->is_string() || packet->as_string().size() > trace_control_packet_bytes ||
      !is_valid_utf8(packet->as_string())) return std::nullopt;
  return TraceControlReply{*generation, *cursor, reset->as_bool(), gap->as_bool(), packet->as_string()};
}
} // namespace layerc::core
