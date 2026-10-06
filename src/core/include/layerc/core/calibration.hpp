#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace layerc::core {

// Profile and visual documents are bounded nested objects (null for an absent
// visual sidecar), using the same encoding as live-edit. The envelope remains
// bounded independently of each embedded document.
inline constexpr std::uint32_t calibration_protocol_version = 2;
inline constexpr std::size_t max_calibration_packet_bytes = 2U * 1024U * 1024U;

struct CalibrationRequest {
  std::uint32_t protocol_version{calibration_protocol_version};
  std::uint64_t target_window{};
  std::string cooldown_id;
  std::string profile_json;
  // Empty means the base profile currently has no visual sidecar.
  std::string visual_json;
};

struct CalibrationResponse {
  std::uint32_t protocol_version{calibration_protocol_version};
  bool applied{};
  // On Apply this is a valid one-observation, one-trigger VisualProfile.
  std::string visual_json;
  std::string error;
};

struct CalibrationProtocolError {
  std::string message;
};

using CalibrationRequestResult =
    std::variant<CalibrationRequest, CalibrationProtocolError>;
using CalibrationResponseResult =
    std::variant<CalibrationResponse, CalibrationProtocolError>;

[[nodiscard]] std::string
encode_calibration_request(const CalibrationRequest &request);
[[nodiscard]] CalibrationRequestResult
decode_calibration_request(std::string_view source);
[[nodiscard]] std::string
encode_calibration_response(const CalibrationResponse &response);
[[nodiscard]] CalibrationResponseResult
decode_calibration_response(std::string_view source);

} // namespace layerc::core
