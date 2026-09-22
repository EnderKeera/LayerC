#include "isolated_visual_protocol.hpp"
#include "test_support.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <variant>

namespace {

using layerc::test::expect;

layerc::daemon::VisualCaptureSnapshot
snapshot(const std::uint64_t capture_sequence,
         const std::uint64_t evaluation_sequence,
         const layerc::core::TimePoint sampled_at) {
  layerc::daemon::VisualCaptureSnapshot value;
  value.sequence = capture_sequence;
  value.gpu_description = "test GPU";
  value.evaluations.push_back({
      .source_key = "profile.json",
      .sequence = evaluation_sequence,
      .measurements = {{.observation_id = "ready",
                        .matched = true,
                        .confidence = 0.95,
                        .sampled_at = sampled_at}},
  });
  return value;
}

void handshake_is_mutually_bound() {
  using namespace layerc::daemon;
  std::string error;
  const auto hello = isolated_hello_json("nonce", "worker", 73);
  expect(
      validate_isolated_hello(hello, "nonce", "worker", 73, error),
      "matching nonce, role, PID, protocol, and package should authenticate");
  error.clear();
  expect(!validate_isolated_hello(hello, "spoof", "worker", 73, error),
         "a spoofed nonce should be rejected");
  error.clear();
  expect(!validate_isolated_hello(hello, "nonce", "daemon", 73, error),
         "a spoofed role should be rejected");

  auto wrong_version = hello;
  const auto protocol = wrong_version.find("\"protocolVersion\":1");
  wrong_version.replace(protocol, 19, "\"protocolVersion\":99");
  error.clear();
  expect(!validate_isolated_hello(wrong_version, "nonce", "worker", 73, error),
         "an incompatible helper protocol should be rejected");

  const std::string oversized(isolated_visual_max_message_bytes + 1U, 'x');
  error.clear();
  expect(!validate_isolated_hello(oversized, "nonce", "worker", 73, error),
         "oversized helper messages should fail before parsing");
}

void configuration_and_packet_sequences_round_trip() {
  using namespace layerc::daemon;
  static_assert(isolated_visual_heartbeat_ms < isolated_visual_watchdog_ms,
                "isolated helper heartbeats must leave watchdog scheduling margin");
  IsolatedConfiguration configuration;
  configuration.sequence = 9'007'199'254'740'993ULL;
  configuration.capture_quality = layerc::core::CaptureQuality::low;
  configuration.exclusions.push_back({1, 2, 30, 40});
  VisualEvaluationRequest request;
  request.source_key = "profile.json";
  layerc::core::TemplateState state{.id = "ready",
                                    .name = "Ready",
                                    .search_region = {0.0, 0.0, 1.0, 1.0},
                                    .expected_width = 0.1,
                                    .expected_height = 0.1,
                                    .references = {{std::string(64, 'a')}}};
  VisualTemplateEvaluation evaluation;
  evaluation.state = std::move(state);
  evaluation.asset_signatures = {{.present = true,
                                  .volume_serial = 7,
                                  .file_index = 11,
                                  .file_size = 13,
                                  .last_write_ticks = 17}};
  request.templates.push_back(std::move(evaluation));
  configuration.requests.push_back(std::move(request));
  const auto decoded = decode_isolated_configuration(
      encode_isolated_configuration(configuration));
  const auto *value = std::get_if<IsolatedConfiguration>(&decoded);
  expect(
      value != nullptr && value->sequence == configuration.sequence &&
          value->capture_quality == configuration.capture_quality &&
          value->exclusions.size() == 1 && value->requests.size() == 1 &&
          value->requests.front().templates.size() == 1 &&
          value->requests.front().templates.front().asset_signatures ==
              configuration.requests.front().templates.front().asset_signatures,
      "bounded helper configuration and asset revisions should round-trip "
      "exactly");

  std::uint64_t sequence{};
  std::string error;
  expect(decode_isolated_poll(encode_isolated_poll(42), sequence, error) &&
             sequence == 42 &&
             encode_isolated_poll(42).find("waitSnapshot") != std::string::npos,
         "event-driven waitSnapshot sequence should round-trip");
  expect(decode_isolated_ack(encode_isolated_ack(42), 42, error),
         "matching acknowledgement should be accepted");
  error.clear();
  expect(!decode_isolated_ack(encode_isolated_ack(41), 42, error),
         "out-of-order acknowledgement should be rejected");
}

void inbound_messages_decode_in_one_pass() {
  using namespace layerc::daemon;
  IsolatedConfiguration configuration;
  configuration.sequence = 7;
  configuration.capture_quality = layerc::core::CaptureQuality::exact;
  auto message = decode_isolated_message(
      encode_isolated_configuration(configuration));
  const auto *decoded =
      std::get_if<IsolatedConfiguration>(&message);
  expect(decoded != nullptr && decoded->sequence == 7 &&
             decoded->requests.empty(),
         "configure packets should decode through the single-parse entry");

  message = decode_isolated_message(encode_isolated_poll(99));
  const auto *poll = std::get_if<IsolatedPoll>(&message);
  expect(poll != nullptr && poll->sequence == 99,
         "waitSnapshot packets should decode through the single-parse entry");

  message = decode_isolated_message(encode_isolated_ack(1));
  expect(std::holds_alternative<IsolatedProtocolError>(message),
         "non-inbound packet types should be rejected by the helper decoder");
}

void worker_sequences_survive_quality_epochs() {
  using namespace layerc::daemon;
  const auto now = layerc::core::MonotonicClock::now();
  IsolatedSnapshotValidator validator;
  auto low = snapshot(1, 1, now);
  expect(validator.rebase(low) && low.sequence == 1 &&
             low.evaluations.front().sequence == 1,
         "the first worker epoch should publish sequence one");
  auto duplicate = snapshot(1, 1, now);
  expect(!validator.rebase(duplicate) && duplicate.sequence == 1 &&
             duplicate.evaluations.front().sequence == 1,
         "duplicate worker snapshots should preserve protocol sequences");
  validator.begin_worker_epoch();
  auto balanced = snapshot(1, 1, now);
  expect(validator.rebase(balanced) && balanced.sequence == 2 &&
             balanced.evaluations.front().sequence == 2,
         "quality-driven worker replacement must keep protocol sequences "
         "monotonic");
}

void snapshots_reject_replay_and_staleness() {
  using namespace layerc::daemon;
  const auto now = layerc::core::MonotonicClock::now();
  auto first = snapshot(1, 1, now);
  const auto encoded = encode_isolated_snapshot(17, first);
  auto decoded = decode_isolated_snapshot(encoded, 17);
  expect(std::holds_alternative<VisualCaptureSnapshot>(decoded),
         "a bounded current snapshot should decode");
  expect(std::holds_alternative<IsolatedProtocolError>(
             decode_isolated_snapshot(encoded, 18)),
         "a snapshot for another poll sequence should be rejected");

  IsolatedSnapshotValidator validator;
  std::string error;
  expect(validator.validate(first, now, error) ==
             IsolatedSnapshotDisposition::accepted,
         "the first fresh capture should be accepted");
  auto duplicate = snapshot(1, 1, now);
  expect(validator.validate(duplicate, now, error) ==
             IsolatedSnapshotDisposition::duplicate,
         "a duplicate capture should not be republished");

  auto partially_new = snapshot(2, 1, now);
  expect(validator.validate(partially_new, now, error) ==
                 IsolatedSnapshotDisposition::accepted &&
             partially_new.evaluations.empty(),
         "duplicate per-profile measurements should be removed from a newer "
         "capture");

  auto stale = snapshot(3, 2, now - std::chrono::seconds(3));
  error.clear();
  expect(validator.validate(stale, now, error) ==
             IsolatedSnapshotDisposition::rejected,
         "stale QPC measurements should be rejected");
  auto backwards = snapshot(1, 2, now);
  error.clear();
  expect(validator.validate(backwards, now, error) ==
             IsolatedSnapshotDisposition::rejected,
         "capture sequences must not move backwards");
}

} // namespace

int main() {
  return layerc::test::run({
      {"handshake_is_mutually_bound", handshake_is_mutually_bound},
      {"configuration_and_packet_sequences_round_trip", configuration_and_packet_sequences_round_trip},
      {"inbound_messages_decode_in_one_pass", inbound_messages_decode_in_one_pass},
      {"worker_sequences_survive_quality_epochs", worker_sequences_survive_quality_epochs},
      {"snapshots_reject_replay_and_staleness", snapshots_reject_replay_and_staleness},
  });
}
