#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/enum_wire.hpp"

namespace layerc::core {
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
[[nodiscard]] std::string serialize_trace_control_reply(const TraceControlReply &);
[[nodiscard]] std::optional<TraceControlReply> parse_trace_control_reply(std::string_view);
} // namespace layerc::core
