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

inline constexpr std::uint32_t isolated_visual_protocol_version = 2;
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
//   asset signature counters) are JSON numbers stored as int64 Values and
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

template <typename T>
using IsolatedProtocolResult = std::variant<T, IsolatedProtocolError>;
using IsolatedStatusResult = IsolatedProtocolResult<std::monostate>;
using IsolatedSnapshotResult = IsolatedProtocolResult<VisualCaptureSnapshot>;
// One parse for every daemon-to-worker packet the helper receives.
using IsolatedInboundMessage =
    std::variant<IsolatedConfiguration, IsolatedPoll, IsolatedProtocolError>;

enum class IsolatedSnapshotDisposition : std::uint8_t {
  accepted,
  duplicate,
};

// Sequence domains of the isolated capture stream. Each side maps counters
// that restart (a recreated capture, a replacement helper connection) onto a
// monotonic stream it owns, so restarts never move published sequences
// backwards and never reuse one.
// - Helper (rebase): the capture sequence advances with each new frame; an
//   evaluation's sequence advances only when that profile was re-sampled, so
//   a slower profile's cached measurement is never presented as fresh.
// - Daemon (validate): every authenticated helper connection is an epoch whose
//   local sequences are validated against that connection only and mapped onto
//   host sequences above everything an earlier helper published.
class IsolatedSnapshotRebaser {
public:
  // The in-process capture was recreated; local counters restart.
  void begin_worker_epoch() noexcept;
  // Map local sequences; true when the capture advanced.
  [[nodiscard]] bool rebase(VisualCaptureSnapshot &snapshot);

private:
  struct EvaluationSequence {
    std::uint64_t local{};
    std::uint64_t published{};
  };

  std::uint64_t local_capture_sequence_{};
  std::uint64_t capture_sequence_{};
  std::map<std::string, EvaluationSequence> evaluations_;
};

class IsolatedSnapshotValidator {
public:
  // Daemon side: a new authenticated helper connection starts its counters.
  void begin_connection() noexcept;
  // Daemon side: accept, deduplicate, or reject a published snapshot.
  [[nodiscard]] IsolatedProtocolResult<IsolatedSnapshotDisposition>
  validate(VisualCaptureSnapshot &snapshot, layerc::core::TimePoint now);

private:
  struct EvaluationSequence {
    std::uint64_t local{};     // last sequence of the restartable source
    std::uint64_t published{}; // last sequence in the owned stream
    std::uint64_t base{};      // daemon: published value at connection start
  };

  std::uint64_t local_capture_sequence_{};
  std::uint64_t capture_sequence_{};
  std::uint64_t capture_base_{};
  std::map<std::string, EvaluationSequence> evaluations_;
};

[[nodiscard]] std::string isolated_hello_json(std::string_view nonce,
                                              std::string_view role,
                                              std::uint32_t process_id);
[[nodiscard]] IsolatedStatusResult
validate_isolated_hello(std::string_view json, std::string_view nonce,
                        std::string_view expected_role,
                        std::uint32_t expected_process_id);
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
[[nodiscard]] IsolatedStatusResult
decode_isolated_ack(std::string_view json, std::uint64_t expected_sequence);

} // namespace layerc::daemon
