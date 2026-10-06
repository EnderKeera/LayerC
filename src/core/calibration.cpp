#include "layerc/core/calibration.hpp"

#include "layerc/core/nested_document.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/visual.hpp"

#include <limits>

namespace layerc::core {
namespace {

using PacketResult = std::variant<json::Value, CalibrationProtocolError>;

[[nodiscard]] PacketResult root_value(const std::string_view source) {
  auto parsed = json::parse_bounded(source, max_calibration_packet_bytes);
  if (const auto *error = std::get_if<json::FieldError>(&parsed))
    return CalibrationProtocolError{error->describe()};
  auto root = std::get<json::Value>(std::move(parsed));
  if (!root.is_object())
    return CalibrationProtocolError{"calibration packet root must be an object"};
  return root;
}

[[nodiscard]] bool current_version(json::Reader &fields, const json::Value &root) {
  return fields.integer(root, "protocolVersion", "$",
                        calibration_protocol_version, calibration_protocol_version,
                        json::Required::yes).has_value();
}

[[nodiscard]] std::string encode_object(json::Value::Object object) {
  std::string output;
  json::append_value(output, json::Value(std::move(object)));
  return output;
}

} // namespace

std::string encode_calibration_request(const CalibrationRequest &request) {
  using json::Value;
  return encode_object({
      {"protocolVersion", Value(static_cast<std::int64_t>(request.protocol_version))},
      {"targetWindow", json::decimal_unsigned_value(request.target_window)},
      {"cooldownId", Value(request.cooldown_id)},
      {"profile", json::nested_document_value(request.profile_json, max_profile_json_bytes)},
      {"visual", json::nested_document_value(request.visual_json, max_visual_json_bytes, true)}});
}

CalibrationRequestResult decode_calibration_request(const std::string_view source) {
  auto parsed = root_value(source);
  if (const auto *error = std::get_if<CalibrationProtocolError>(&parsed))
    return *error;
  const auto &root = std::get<json::Value>(parsed);
  CalibrationRequest request;
  json::Reader fields;
  if (!current_version(fields, root))
    return CalibrationProtocolError{"unsupported calibration protocol version"};
  const auto window = fields.unsigned_integer(
      root, "targetWindow", "$", std::numeric_limits<std::uint64_t>::max(),
      json::Required::yes, 0, json::IntegerEncoding::decimal_string);
  if (!window ||
      !fields.exact_fields(root, {"protocolVersion", "targetWindow", "cooldownId",
                                  "profile", "visual"}, "$") ||
      !fields.string(root, "cooldownId", "$", request.cooldown_id,
                     json::Required::yes, json::EmptyString::reject) ||
      !json::read_nested_document(fields, root, "profile", request.profile_json,
                                  max_profile_json_bytes) ||
      !json::read_nested_document(fields, root, "visual", request.visual_json,
                                  max_visual_json_bytes, true))
    return CalibrationProtocolError{fields.error().describe()};
  request.target_window = *window;
  return request;
}

std::string encode_calibration_response(const CalibrationResponse &response) {
  using json::Value;
  return encode_object({
      {"protocolVersion", Value(static_cast<std::int64_t>(response.protocol_version))},
      {"applied", Value(response.applied)},
      {"visual", json::nested_document_value(response.visual_json, max_visual_json_bytes, true)},
      {"error", Value(response.error)}});
}

CalibrationResponseResult decode_calibration_response(const std::string_view source) {
  auto parsed = root_value(source);
  if (const auto *error = std::get_if<CalibrationProtocolError>(&parsed))
    return *error;
  const auto &root = std::get<json::Value>(parsed);
  CalibrationResponse response;
  json::Reader fields;
  if (!current_version(fields, root))
    return CalibrationProtocolError{"unsupported calibration protocol version"};
  if (!fields.exact_fields(root, {"protocolVersion", "applied", "visual", "error"}, "$") ||
      !fields.boolean(root, "applied", "$", response.applied, json::Required::yes) ||
      !json::read_nested_document(fields, root, "visual", response.visual_json,
                                  max_visual_json_bytes, true) ||
      !fields.string(root, "error", "$", response.error,
                     json::Required::yes, json::EmptyString::allow))
    return CalibrationProtocolError{fields.error().describe()};
  if (response.applied && response.visual_json.empty())
    return CalibrationProtocolError{"applied calibration response has no visual observation"};
  return response;
}

} // namespace layerc::core
