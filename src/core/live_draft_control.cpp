#include "layerc/core/live_draft_control.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/utf8.hpp"

#include <algorithm>
#include <limits>

namespace layerc::core {
namespace {
inline constexpr std::size_t max_profile_id_bytes = 256;

bool version_ok(const json::Value &v) {
  const auto *version = v.find("draftVersion");
  return version && version->is_integer() && version->as_integer() == live_draft_version;
}
std::optional<std::uint64_t> number(const json::Value &v, const std::string_view name) {
  const auto *value = v.find(name);
  if (!value || !value->is_integer() || value->as_integer() < 0) return std::nullopt;
  return static_cast<std::uint64_t>(value->as_integer());
}
} // namespace

bool live_draft_request_valid(const LiveDraftRequest &request) noexcept {
  const bool client_ok = !request.client_id.empty() &&
      request.client_id.size() <= live_draft_client_bytes &&
      std::ranges::all_of(request.client_id, [](const char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
      });
  const bool publish = request.operation == LiveDraftOperation::publish;
  const bool body_ok = publish
      ? !request.profile_json.empty() && request.profile_json.size() <= live_draft_profile_bytes &&
            is_valid_utf8(request.profile_json)
      : request.profile_json.empty();
  return client_ok && body_ok && !request.profile_id.empty() &&
         request.profile_id.size() <= max_profile_id_bytes && is_valid_utf8(request.profile_id) &&
         enum_wire_name(live_draft_operation_wire, request.operation).has_value() &&
         request.revision <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
         (request.operation == LiveDraftOperation::end || request.revision != 0);
}

std::string serialize_live_draft_request(const LiveDraftRequest &request) {
  std::string out = "{\"draftVersion\":1,\"operation\":";
  json::append_quoted(out, enum_wire_name(live_draft_operation_wire, request.operation).value_or(""));
  out += ",\"profileId\":"; json::append_quoted(out, request.profile_id);
  out += ",\"clientId\":"; json::append_quoted(out, request.client_id);
  out += ",\"revision\":"; json::append_unsigned(out, request.revision);
  out += ",\"profileJson\":"; json::append_quoted(out, request.profile_json);
  out += "}";
  return out;
}

std::optional<LiveDraftRequest> parse_live_draft_request(const std::string_view text) {
  // A quoted profile can double in size when every byte is escaped.
  if (text.size() > 6U * live_draft_profile_bytes + 4096U) return std::nullopt;
  const auto parsed = json::parse(text);
  const auto *value = std::get_if<json::Value>(&parsed);
  return value ? parse_live_draft_request(*value) : std::nullopt;
}
std::optional<LiveDraftRequest> parse_live_draft_request(const json::Value &value) {
  const auto *v = &value;
  if (!v->is_object() || v->as_object().size() != 6 || !version_ok(*v)) return std::nullopt;
  const auto *op = v->find("operation"), *profile = v->find("profileId"),
             *client = v->find("clientId"), *body = v->find("profileJson");
  const auto revision = number(*v, "revision");
  if (!op || !op->is_string() || !profile || !profile->is_string() || !client ||
      !client->is_string() || !body || !body->is_string() || !revision) return std::nullopt;
  const auto operation = parse_enum_wire(live_draft_operation_wire, op->as_string());
  if (!operation) return std::nullopt;
  LiveDraftRequest request{*operation, profile->as_string(), client->as_string(), *revision,
                           body->as_string()};
  if (!live_draft_request_valid(request)) return std::nullopt;
  return request;
}

std::string serialize_live_draft_reply(const LiveDraftReply &reply) {
  std::string out = "{\"draftVersion\":1,\"revision\":";
  json::append_unsigned(out, reply.revision);
  out += ",\"state\":";
  json::append_quoted(out, enum_wire_name(live_draft_state_wire, reply.state).value_or(""));
  out += "}";
  return out;
}

std::optional<LiveDraftReply> parse_live_draft_reply(const std::string_view text) {
  if (text.size() > 1024U) return std::nullopt;
  const auto parsed = json::parse(text);
  const auto *value = std::get_if<json::Value>(&parsed);
  return value ? parse_live_draft_reply(*value) : std::nullopt;
}
std::optional<LiveDraftReply> parse_live_draft_reply(const json::Value &value) {
  const auto *v = &value;
  if (!v->is_object() || v->as_object().size() != 3 || !version_ok(*v)) return std::nullopt;
  const auto revision = number(*v, "revision");
  const auto *state = v->find("state");
  if (!revision || !state || !state->is_string()) return std::nullopt;
  const auto parsed_state = parse_enum_wire(live_draft_state_wire, state->as_string());
  if (!parsed_state) return std::nullopt;
  return LiveDraftReply{*revision, *parsed_state};
}
LiveDraftFailure live_draft_failure(const LiveDraftError code) {
  switch (code) {
  case LiveDraftError::invalid_request:
    return {code, "invalid live draft request"};
  case LiveDraftError::unavailable:
    return {code, "live drafts unavailable"};
  case LiveDraftError::owned_elsewhere:
    return {code, "another editor owns the live draft"};
  case LiveDraftError::clock_limit:
    return {code, "live draft lease clock limit exceeded"};
  case LiveDraftError::expired:
    return {code, "live draft expired; publish it again"};
  case LiveDraftError::revision_mismatch:
    return {code, "live draft revision mismatch"};
  case LiveDraftError::stale_revision:
    return {code, "stale live draft revision"};
  case LiveDraftError::invalid_profile:
    return {code, "invalid live draft profile: "};
  case LiveDraftError::id_mismatch:
    return {code, "live draft profile id mismatch"};
  case LiveDraftError::unknown_profile:
    return {code, "live draft names no stored profile; Save creates it first"};
  }
  return {code, "unknown live draft error"};
}

} // namespace layerc::core
