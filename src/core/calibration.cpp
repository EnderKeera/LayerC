#include "layerc/core/calibration.hpp"

#include "layerc/core/json_reader.hpp"

#include <limits>

namespace layerc::core {
namespace {

CalibrationProtocolError error(std::string message) {
  return {std::move(message)};
}


const json::Value *root_value(const std::string_view source,
                              json::ParseResult &parsed,
                              CalibrationProtocolError &failure) {
  if (source.size() > max_calibration_packet_bytes) {
    failure = error("calibration packet exceeds 2 MiB");
    return nullptr;
  }
  parsed = json::parse(source);
  if (const auto *parse_error = std::get_if<json::ParseError>(&parsed)) {
    failure = error("invalid calibration JSON at byte " +
                    std::to_string(parse_error->offset) + ": " +
                    parse_error->message);
    return nullptr;
  }
  const auto &root = std::get<json::Value>(parsed);
  if (!root.is_object()) {
    failure = error("calibration packet root must be an object");
    return nullptr;
  }
  return &root;
}

} // namespace

std::string encode_calibration_request(const CalibrationRequest &request) {
  std::string output = "{\"protocolVersion\":1,\"targetWindow\":\"" +
                       std::to_string(request.target_window) +
                       "\",\"cooldownId\":";
  json::append_quoted(output, request.cooldown_id);
  output += ",\"profileJson\":";
  json::append_quoted(output, request.profile_json);
  output += ",\"visualJson\":";
  json::append_quoted(output, request.visual_json);
  output.push_back('}');
  return output;
}

CalibrationRequestResult
decode_calibration_request(const std::string_view source) {
  json::ParseResult parsed;
  CalibrationProtocolError failure;
  const auto *root = root_value(source, parsed, failure);
  if (root == nullptr) return failure;
  CalibrationRequest request;
  json::Reader fields;
  if (!fields.integer(*root, "protocolVersion", "$", 1, 1, json::Required::yes))
    return error("unsupported calibration protocol version");
  const auto window = fields.unsigned_integer(
      *root, "targetWindow", "$", std::numeric_limits<std::uint64_t>::max(),
      json::Required::yes, 0, json::IntegerEncoding::decimal_string);
  if (!window ||
      !fields.string(*root, "cooldownId", "$", request.cooldown_id,
                     json::Required::yes, json::EmptyString::reject) ||
      !fields.string(*root, "profileJson", "$", request.profile_json,
                     json::Required::yes, json::EmptyString::allow) ||
      !fields.string(*root, "visualJson", "$", request.visual_json,
                     json::Required::no, json::EmptyString::allow))
    return error(fields.error().describe());
  request.target_window = *window;
  return request;
}

std::string encode_calibration_response(const CalibrationResponse &response) {
  std::string output = "{\"protocolVersion\":1,\"applied\":";
  json::append_bool(output, response.applied);
  output += ",\"visualJson\":";
  json::append_quoted(output, response.visual_json);
  output += ",\"error\":";
  json::append_quoted(output, response.error);
  output.push_back('}');
  return output;
}

CalibrationResponseResult
decode_calibration_response(const std::string_view source) {
  json::ParseResult parsed;
  CalibrationProtocolError failure;
  const auto *root = root_value(source, parsed, failure);
  if (root == nullptr) return failure;
  CalibrationResponse response;
  json::Reader fields;
  if (!fields.integer(*root, "protocolVersion", "$", 1, 1, json::Required::yes))
    return error("unsupported calibration protocol version");
  if (!fields.boolean(*root, "applied", "$", response.applied, json::Required::yes) ||
      !fields.string(*root, "visualJson", "$", response.visual_json,
                     json::Required::no, json::EmptyString::allow) ||
      !fields.string(*root, "error", "$", response.error,
                     json::Required::no, json::EmptyString::allow))
    return error(fields.error().describe());
  if (response.applied && response.visual_json.empty())
    return error("applied calibration response has no visual observation");
  return response;
}

} // namespace layerc::core
