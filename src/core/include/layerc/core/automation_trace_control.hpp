#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/enum_wire.hpp"

namespace layerc::core {
namespace json {
class Value;
}

// A typed, versioned polling payload for authenticated local transports.
// Neither the payload nor this codec owns sockets, input, or platform handles.
inline constexpr std::uint32_t trace_control_version = 1;
inline constexpr std::size_t trace_control_packet_bytes = 128U * 1024U;
inline constexpr std::size_t trace_control_client_bytes = 64;
inline constexpr std::size_t trace_control_client_limit = 16;
inline constexpr std::size_t trace_control_batch_entries = 32;
inline constexpr Milliseconds trace_control_lease{2500};

enum class TraceControlOperation : std::uint8_t { subscribe, poll, unsubscribe };
inline constexpr std::array trace_control_operation_wire{
    EnumWireEntry{TraceControlOperation::subscribe, "subscribe"},
    EnumWireEntry{TraceControlOperation::poll, "poll"},
    EnumWireEntry{TraceControlOperation::unsubscribe, "unsubscribe"},
};
// Machine-readable control failures; the text stays presentation only.
enum class TraceControlError : std::uint8_t {
  invalid_request,
  unavailable,
  client_conflict,
  subscriber_limit,
  clock_limit,
  expired,
  invalid_cursor,
};
inline constexpr std::array trace_control_error_wire{
    EnumWireEntry{TraceControlError::invalid_request, "invalid-request"},
    EnumWireEntry{TraceControlError::unavailable, "unavailable"},
    EnumWireEntry{TraceControlError::client_conflict, "client-conflict"},
    EnumWireEntry{TraceControlError::subscriber_limit, "subscriber-limit"},
    EnumWireEntry{TraceControlError::clock_limit, "clock-limit"},
    EnumWireEntry{TraceControlError::expired, "expired"},
    EnumWireEntry{TraceControlError::invalid_cursor, "invalid-cursor"},
};
struct TraceControlFailure {
  TraceControlError code{};
  std::string message;
  bool operator==(const TraceControlFailure &) const = default;
};
// A lapsed lease or cursor: the client resubscribes and reports a gap.
[[nodiscard]] constexpr bool trace_control_lapsed(const TraceControlError error) noexcept {
  return error == TraceControlError::expired || error == TraceControlError::invalid_cursor;
}
[[nodiscard]] TraceControlFailure trace_control_failure(TraceControlError code);
struct TraceControlRequest {
  TraceControlOperation operation{TraceControlOperation::subscribe};
  std::string profile_id;
  std::string client_id;
  std::uint64_t generation{};
  std::uint64_t cursor{};
  bool operator==(const TraceControlRequest &) const = default;
};
struct TraceControlReply {
  std::uint64_t generation{};
  std::uint64_t cursor{};
  bool reset{};
  bool gap{};
  std::string packet;
  bool operator==(const TraceControlReply &) const = default;
};
[[nodiscard]] bool trace_control_request_valid(const TraceControlRequest &) noexcept;
[[nodiscard]] std::string serialize_trace_control_request(const TraceControlRequest &);
[[nodiscard]] std::optional<TraceControlRequest> parse_trace_control_request(std::string_view);
[[nodiscard]] std::optional<TraceControlRequest>
parse_trace_control_request(const json::Value &value);
[[nodiscard]] std::string serialize_trace_control_reply(const TraceControlReply &);
[[nodiscard]] std::optional<TraceControlReply> parse_trace_control_reply(std::string_view);
[[nodiscard]] std::optional<TraceControlReply> parse_trace_control_reply(const json::Value &value);
} // namespace layerc::core
