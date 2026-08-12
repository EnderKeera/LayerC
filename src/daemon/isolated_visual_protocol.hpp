#pragma once

#include "visual_capture_worker.hpp"
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
inline constexpr std::size_t isolated_visual_max_queued_bytes =
    4U * isolated_visual_max_message_bytes;

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

using IsolatedConfigurationResult =
    std::variant<IsolatedConfiguration, IsolatedProtocolError>;
using IsolatedSnapshotResult =
    std::variant<VisualCaptureSnapshot, IsolatedProtocolError>;

enum class IsolatedSnapshotDisposition : std::uint8_t {
  accepted,
  duplicate,
  rejected,
};

class IsolatedSnapshotValidator {
public:
  [[nodiscard]] IsolatedSnapshotDisposition
  validate(VisualCaptureSnapshot &snapshot, layerc::core::TimePoint now,
           std::string &error);

private:
  std::uint64_t capture_sequence_{};
  std::map<std::wstring, std::uint64_t> evaluation_sequences_;
};

// VisualCaptureWorker sequence counters restart when its capture quality
// changes. This protocol-level sequencer keeps the helper's published stream
// monotonic across those worker epochs.
class IsolatedSnapshotSequencer {
public:
  void begin_worker_epoch() noexcept { last_worker_sequence_ = 0; }
  [[nodiscard]] bool rebase(VisualCaptureSnapshot &snapshot);

private:
  std::uint64_t last_worker_sequence_{};
  std::uint64_t capture_sequence_{};
  std::map<std::wstring, std::uint64_t> evaluation_sequences_;
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
[[nodiscard]] IsolatedConfigurationResult
decode_isolated_configuration(std::string_view json);
[[nodiscard]] std::string encode_isolated_poll(std::uint64_t sequence);
[[nodiscard]] bool decode_isolated_poll(std::string_view json,
                                        std::uint64_t &sequence,
                                        std::string &error);
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
