#include "layerc/core/calibration.hpp"

#include "layerc/core/json.hpp"

#include <charconv>
#include <cmath>
#include <limits>

namespace layerc::core {
namespace {

CalibrationProtocolError error(std::string message) {
  return {std::move(message)};
}

bool version(const json::Value &root, std::uint32_t &output,
             std::string &failure) {
  const auto *value = root.find("protocolVersion");
  if (value == nullptr || !value->is_number() ||
      !std::isfinite(value->as_number()) ||
      std::floor(value->as_number()) != value->as_number() ||
      value->as_number() != 1.0) {
    failure = "unsupported calibration protocol version";
    return false;
  }
  output = 1;
  return true;
}

bool string_field(const json::Value &root, const std::string_view name,
                  std::string &output, std::string &failure,
                  const bool required) {
  const auto *value = root.find(name);
  if (value == nullptr) {
    if (required) failure = std::string(name) + " is required";
    return !required;
  }
  if (!value->is_string()) {
    failure = std::string(name) + " must be a string";
    return false;
  }
  output = value->as_string();
  return true;
}

bool decimal_u64(const json::Value &root, const std::string_view name,
                 std::uint64_t &output, std::string &failure) {
  std::string text;
  if (!string_field(root, name, text, failure, true) || text.empty())
    return false;
  const auto [tail, error] =
      std::from_chars(text.data(), text.data() + text.size(), output);
  if (error != std::errc{} || tail != text.data() + text.size()) {
    failure = std::string(name) + " is outside the supported range";
    return false;
  }
  return true;
}

bool bool_field(const json::Value &root, const std::string_view name,
                bool &output, std::string &failure) {
  const auto *value = root.find(name);
  if (value == nullptr || !value->is_bool()) {
    failure = std::string(name) + " must be boolean";
    return false;
  }
  output = value->as_bool();
  return true;
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
  std::string message;
  if (!version(*root, request.protocol_version, message) ||
      !decimal_u64(*root, "targetWindow", request.target_window, message) ||
      !string_field(*root, "cooldownId", request.cooldown_id, message, true) ||
      request.cooldown_id.empty() ||
      !string_field(*root, "profileJson", request.profile_json, message, true) ||
      !string_field(*root, "visualJson", request.visual_json, message, false))
    return error(std::move(message));
  return request;
}

std::string encode_calibration_response(const CalibrationResponse &response) {
  std::string output = "{\"protocolVersion\":1,\"applied\":";
  output += response.applied ? "true" : "false";
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
  std::string message;
  if (!version(*root, response.protocol_version, message) ||
      !bool_field(*root, "applied", response.applied, message) ||
      !string_field(*root, "visualJson", response.visual_json, message, false) ||
      !string_field(*root, "error", response.error, message, false))
    return error(std::move(message));
  if (response.applied && response.visual_json.empty())
    return error("applied calibration response has no visual observation");
  return response;
}

} // namespace layerc::core
