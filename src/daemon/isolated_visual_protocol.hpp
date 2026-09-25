#pragma once

#include "visual_capture_host.hpp"
#include "layerc/core/live_edit.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::daemon {

inline constexpr std::uint32_t isolated_visual_protocol_version = 1;
inline constexpr std::size_t isolated_visual_max_message_bytes =
    1024U * 1024U;
inline constexpr std::uint32_t isolated_visual_heartbeat_ms = 1'000;
inline constexpr std::uint32_t isolated_visual_watchdog_ms = 2'000;
static_assert(isolated_visual_heartbeat_ms < isolated_visual_watchdog_ms);

// The isolated Windows helper is WGC / Desktop Duplication only. X11 backends
// stay out of this wire; the bound comes from the core enumerator, not a magic
// ordinal.
inline constexpr std::uint8_t isolated_capture_backend_max =
    static_cast<std::uint8_t>(
        layerc::core::CaptureBackendKind::desktop_duplication);

// Wire policy (both ends ship in one package; packageVersion rejects any other
// build, so there is no mixed-version compatibility path):
// - Integer fields (sequence, processId, captureSequence, sampledAtNs, and
//   asset signature counters) are JSON numbers via json::append_unsigned and
//   are read as IntegerEncoding::integer_token.
// - Request visual profiles travel as a nested visualProfile object produced
//   by serialize_visual_profile_json (not a double-encoded visualJson string)
//   and are read with parse_visual_profile_value.

struct IsolatedProtocolError {
  std::string message;
};

struct IsolatedConfiguration {
  std::uint64_t sequence{};
  layerc::core::CaptureQuality capture_quality{
      layerc::core::CaptureQuality::balanced};
  std::vector<VisualEvaluationRequest> requests;
  std::vector<RECT> exclusions;
};

struct IsolatedPoll {
  std::uint64_t sequence{};
};

using IsolatedSnapshotResult =
    std::variant<VisualCaptureSnapshot, IsolatedProtocolError>;
// One parse for every daemon-to-worker packet the helper receives.
using IsolatedInboundMessage =
    std::variant<IsolatedConfiguration, IsolatedPoll, IsolatedProtocolError>;

enum class IsolatedSnapshotDisposition : std::uint8_t {
  accepted,
  duplicate,
  rejected,
};

// Shared sequence counters for both sides of the isolated capture stream.
// The helper rebases its local capture counter onto the published monotonic
// stream; the daemon validates and deduplicates what it receives. A helper
// can recreate its in-process capture after an empty configuration — epoch
// reset lives here so those restarts cannot move published sequences backwards.
class IsolatedSnapshotValidator {
public:
  void begin_worker_epoch() noexcept { last_worker_sequence_ = 0; }
  // Worker side: rebase helper-local sequences; true when the capture advanced.
  [[nodiscard]] bool rebase(VisualCaptureSnapshot &snapshot);
  // Daemon side: accept, deduplicate, or reject a published snapshot.
  [[nodiscard]] IsolatedSnapshotDisposition
  validate(VisualCaptureSnapshot &snapshot, layerc::core::TimePoint now,
           std::string &error);

private:
  std::uint64_t last_worker_sequence_{};
  std::uint64_t capture_sequence_{};
  std::map<std::string, std::uint64_t> evaluation_sequences_;
};

[[nodiscard]] std::string isolated_hello_json(std::string_view nonce,
                                              std::string_view role,
                                              std::uint32_t process_id);
[[nodiscard]] bool validate_isolated_hello(std::string_view json,
                                           std::string_view nonce,
                                           std::string_view expected_role,
                                           std::uint32_t expected_process_id,
                                           std::string &error);
[[nodiscard]] std::string
encode_isolated_configuration(const IsolatedConfiguration &configuration);
[[nodiscard]] std::string encode_isolated_poll(std::uint64_t sequence);
// Single-parse entry for helper inbound packets (configure / waitSnapshot).
[[nodiscard]] IsolatedInboundMessage
decode_isolated_message(std::string_view json);
[[nodiscard]] std::string
encode_isolated_snapshot(std::uint64_t packet_sequence,
                         const VisualCaptureSnapshot &snapshot);
[[nodiscard]] IsolatedSnapshotResult
decode_isolated_snapshot(std::string_view json,
                         std::uint64_t expected_packet_sequence);
[[nodiscard]] std::string encode_isolated_ack(std::uint64_t sequence);
[[nodiscard]] bool decode_isolated_ack(std::string_view json,
                                       std::uint64_t expected_sequence,
                                       std::string &error);

} // namespace layerc::daemon
