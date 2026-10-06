#include "isolated_visual_protocol.hpp"
#include "layerc/core/json.hpp"
#include "test_support.hpp"

#include <chrono>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>

void snapshot_capture_health_round_trips();
void capture_faults_do_not_fault_unrelated_services();
void inline_capture_status_preserves_fault_reason();

namespace {

using layerc::test::expect;
using layerc::core::json::Value;

Value::Object packet_object(const std::string &encoded) {
  return std::get<Value>(layerc::core::json::parse(encoded)).as_object();
}

std::string packet_json(Value::Object packet) {
  std::string encoded;
  layerc::core::json::append_value(encoded, Value(std::move(packet)));
  return encoded;
}

template <typename Result>
void expect_protocol_error(const Result &result, const std::string_view detail) {
  const auto *error =
      std::get_if<layerc::daemon::IsolatedProtocolError>(&result);
  expect(error != nullptr && !error->message.empty() &&
             error->message.find(detail) != std::string::npos,
         "a decoder failure must carry the original typed error detail");
}

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

// The validation disposition, or nullopt when the snapshot was rejected.
std::optional<layerc::daemon::IsolatedSnapshotDisposition>
verdict(layerc::daemon::IsolatedSnapshotValidator &validator,
        layerc::daemon::VisualCaptureSnapshot &value,
        const layerc::core::TimePoint now) {
  const auto result = validator.validate(value, now);
  if (const auto *disposition =
          std::get_if<layerc::daemon::IsolatedSnapshotDisposition>(&result))
    return *disposition;
  return std::nullopt;
}

void handshake_is_mutually_bound() {
  using namespace layerc::daemon;
  const auto hello = isolated_hello_json("nonce", "worker", 73);
  expect(std::holds_alternative<std::monostate>(
             validate_isolated_hello(hello, "nonce", "worker", 73)),
         "matching nonce, role, PID, protocol, and package should authenticate");
  expect(std::holds_alternative<IsolatedProtocolError>(
             validate_isolated_hello(hello, "spoof", "worker", 73)),
         "a spoofed nonce should be rejected");
  expect(std::holds_alternative<IsolatedProtocolError>(
             validate_isolated_hello(hello, "nonce", "daemon", 73)),
         "a spoofed role should be rejected");
  expect(std::holds_alternative<IsolatedProtocolError>(
             validate_isolated_hello(hello, "nonce", "worker", 74)),
         "a spoofed PID should be rejected");

  auto wrong_version = hello;
  const auto version_field = "\"protocolVersion\":" +
                             std::to_string(isolated_visual_protocol_version);
  const auto protocol = wrong_version.find(version_field);
  wrong_version.replace(protocol, version_field.size(), "\"protocolVersion\":99");
  expect(std::holds_alternative<IsolatedProtocolError>(
             validate_isolated_hello(wrong_version, "nonce", "worker", 73)),
         "an incompatible helper protocol should be rejected");

  const std::string oversized(isolated_visual_max_message_bytes + 1U, 'x');
  expect(std::holds_alternative<IsolatedProtocolError>(
             validate_isolated_hello(oversized, "nonce", "worker", 73)),
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
                                  .file_index = 9'007'199'254'740'993ULL,
                                  .file_size = 13,
                                  .last_write_ticks = 17,
                                  .change_usn = 19}};
  request.templates.push_back(std::move(evaluation));
  configuration.requests.push_back(std::move(request));
  const auto decoded = decode_isolated_message(
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

  const auto poll = decode_isolated_message(encode_isolated_poll(42));
  const auto *polled = std::get_if<IsolatedPoll>(&poll);
  expect(polled != nullptr && polled->sequence == 42 &&
             encode_isolated_poll(42).find("waitSnapshot") != std::string::npos,
         "event-driven waitSnapshot sequence should round-trip");
  expect(std::holds_alternative<std::monostate>(
             decode_isolated_ack(encode_isolated_ack(42), 42)),
         "matching acknowledgement should be accepted");
  expect(std::holds_alternative<IsolatedProtocolError>(
             decode_isolated_ack(encode_isolated_ack(41), 42)),
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
  IsolatedSnapshotRebaser rebaser;
  auto low = snapshot(1, 1, now);
  expect(rebaser.rebase(low) && low.sequence == 1 &&
             low.evaluations.front().sequence == 1,
         "the first worker epoch should publish sequence one");
  auto duplicate = snapshot(1, 1, now);
  expect(!rebaser.rebase(duplicate) && duplicate.sequence == 1 &&
             duplicate.evaluations.front().sequence == 1,
         "duplicate worker snapshots should preserve protocol sequences");
  rebaser.begin_worker_epoch();
  VisualCaptureSnapshot empty;
  expect(!rebaser.rebase(empty) && empty.sequence == 1 &&
             empty.evaluations.empty(),
         "an empty replacement epoch must not manufacture a fresh frame");
  rebaser.begin_worker_epoch();
  auto balanced = snapshot(1, 1, now);
  expect(rebaser.rebase(balanced) && balanced.sequence == 2 &&
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
  expect(verdict(validator, first, now) ==
             IsolatedSnapshotDisposition::accepted,
         "the first fresh capture should be accepted");
  auto duplicate = snapshot(1, 1, now);
  expect(verdict(validator, duplicate, now) ==
             IsolatedSnapshotDisposition::duplicate,
         "a duplicate capture should not be republished");

  auto partially_new = snapshot(2, 1, now);
  expect(verdict(validator, partially_new, now) ==
                 IsolatedSnapshotDisposition::accepted &&
             partially_new.evaluations.empty(),
         "duplicate per-profile measurements should be removed from a newer "
         "capture");

  auto stale = snapshot(3, 2, now - std::chrono::seconds(3));
  expect(verdict(validator, stale, now) ==
             std::nullopt,
         "stale QPC measurements should be rejected");
  auto backwards = snapshot(1, 2, now);
  expect(verdict(validator, backwards, now) ==
             std::nullopt,
         "capture sequences must not move backwards");
}

void replacement_helpers_start_new_epochs() {
  using namespace layerc::daemon;
  const auto now = layerc::core::MonotonicClock::now();
  IsolatedSnapshotValidator daemon;
  daemon.begin_connection();
  auto first = snapshot(99, 99, now);
  auto latest = snapshot(100, 100, now);
  expect(verdict(daemon, first, now) ==
                 IsolatedSnapshotDisposition::accepted &&
             verdict(daemon, latest, now) ==
                 IsolatedSnapshotDisposition::accepted &&
             latest.sequence == 100,
         "helper A publishes a long-running stream");

  // Helper A died; the authenticated replacement B restarts its counters.
  daemon.begin_connection();
  auto replacement = snapshot(1, 1, now);
  expect(verdict(daemon, replacement, now) ==
                 IsolatedSnapshotDisposition::accepted &&
             replacement.sequence == 101 &&
             replacement.evaluations.size() == 1 &&
             replacement.evaluations.front().sequence == 101,
         "a replacement helper's first measurement is accepted above every "
         "sequence helper A published");
  auto repeated = snapshot(1, 1, now);
  expect(verdict(daemon, repeated, now) ==
             IsolatedSnapshotDisposition::duplicate,
         "the replacement's first measurement is delivered exactly once");
  auto backwards = snapshot(1, 2, now);
  auto advanced = snapshot(2, 2, now);
  expect(verdict(daemon, advanced, now) ==
                 IsolatedSnapshotDisposition::accepted &&
             verdict(daemon, backwards, now) ==
                 std::nullopt,
         "sequences still cannot move backwards within one helper epoch");

  // Explicit retry after that fault launches helper C: it recovers at once.
  daemon.begin_connection();
  auto retried = snapshot(1, 1, now);
  expect(verdict(daemon, retried, now) ==
                 IsolatedSnapshotDisposition::accepted &&
             retried.sequence > advanced.sequence &&
             retried.evaluations.front().sequence >
                 advanced.evaluations.front().sequence,
         "retry recovers without another quarantine cycle or reused "
         "sequences");
}

layerc::daemon::VisualEvaluationSnapshot
evaluation(std::string key, const std::uint64_t sequence,
           const layerc::core::TimePoint sampled_at) {
  return {.source_key = std::move(key),
          .sequence = sequence,
          .measurements = {{.observation_id = "ready",
                            .matched = true,
                            .confidence = 0.95,
                            .sampled_at = sampled_at}}};
}

void cached_evaluations_are_not_republished_as_fresh() {
  using namespace layerc::daemon;
  const auto now = layerc::core::MonotonicClock::now();
  IsolatedSnapshotRebaser helper;
  IsolatedSnapshotValidator daemon;
  daemon.begin_connection();
  // Capture 1 samples a 15 Hz and a 5 Hz profile.
  VisualCaptureSnapshot frame;
  frame.sequence = 1;
  frame.evaluations = {evaluation("fast.json", 1, now),
                       evaluation("slow.json", 1, now)};
  expect(helper.rebase(frame) &&
             verdict(daemon, frame, now) ==
                 IsolatedSnapshotDisposition::accepted &&
             frame.evaluations.size() == 2,
         "both profiles' first samples reach the daemon");
  // Capture 2 re-samples only the fast profile; the slow profile keeps its
  // cached measurement, local sequence, and timestamp.
  VisualCaptureSnapshot next;
  next.sequence = 2;
  next.evaluations = {evaluation("fast.json", 2, now),
                      evaluation("slow.json", 1, now)};
  expect(helper.rebase(next) &&
             verdict(daemon, next, now) ==
                 IsolatedSnapshotDisposition::accepted &&
             next.evaluations.size() == 1 &&
             next.evaluations.front().source_key == "fast.json",
         "only the genuinely re-sampled evaluation reaches the daemon");
}

void decoder_errors_are_single_typed_results() {
  using namespace layerc::daemon;
  for (const auto &malformed : {std::string{"{"}, std::string{"[]"},
                                std::string{"{}"}}) {
    expect_protocol_error(
        validate_isolated_hello(malformed, "nonce", "worker", 73), "$");
    expect_protocol_error(decode_isolated_ack(malformed, 7), "$");
    expect_protocol_error(decode_isolated_message(malformed), "$");
    expect_protocol_error(decode_isolated_snapshot(malformed, 7), "$");
  }
  const auto valid = packet_object(encode_isolated_poll(7));
  for (const auto &invalid : {Value(std::string{"7"}), Value(7.5),
                              Value(std::int64_t{-1}), Value()}) {
    auto packet = valid;
    packet.insert_or_assign("sequence", invalid);
    expect_protocol_error(decode_isolated_message(packet_json(std::move(packet))),
                          "$.sequence");
  }
  auto packet = valid;
  packet.erase("sequence");
  expect_protocol_error(decode_isolated_message(packet_json(std::move(packet))),
                        "$.sequence");
  packet = valid;
  packet.insert_or_assign("packageVersion", Value(std::string{"wrong-package"}));
  expect_protocol_error(decode_isolated_message(packet_json(std::move(packet))),
                        "$.packageVersion");
  packet = valid;
  packet.insert_or_assign("type", Value(std::string{"snapshot"}));
  expect_protocol_error(decode_isolated_message(packet_json(std::move(packet))),
                        "unexpected isolated visual message type");
  const auto maximum = static_cast<std::uint64_t>(
      std::numeric_limits<std::int64_t>::max());
  const auto decoded = decode_isolated_message(encode_isolated_poll(maximum));
  const auto *poll = std::get_if<IsolatedPoll>(&decoded);
  expect(poll != nullptr && poll->sequence == maximum,
         "the largest accepted integer token must not lose precision");
  expect(std::holds_alternative<std::monostate>(
             decode_isolated_ack(encode_isolated_ack(maximum), maximum)),
         "a decoder failure must not poison the next valid acknowledgement");
}

void nested_configuration_errors_preserve_detail_and_bounds() {
  using namespace layerc::daemon;
  IsolatedConfiguration configuration;
  configuration.sequence = 7;
  VisualEvaluationRequest request;
  request.source_key = "profile.json";
  VisualTemplateEvaluation evaluation;
  evaluation.state = {.id = "ready", .name = "Ready",
                      .search_region = {0.0, 0.0, 1.0, 1.0},
                      .expected_width = 0.1, .expected_height = 0.1,
                      .references = {{std::string(64, 'a')}}};
  evaluation.asset_signatures.push_back({});
  request.templates.push_back(std::move(evaluation));
  configuration.requests.push_back(std::move(request));
  const auto valid = packet_object(encode_isolated_configuration(configuration));
  auto requests = valid.at("requests").as_array();
  auto encoded_request = requests.front().as_object();
  auto signature_sets = encoded_request.at("assetSignatures").as_array();
  auto signatures = signature_sets.front().as_array();
  auto signature = signatures.front().as_object();
  signature.erase("fileSize");
  signatures.front() = Value(std::move(signature));
  signature_sets.front() = Value(std::move(signatures));
  encoded_request.insert_or_assign("assetSignatures", Value(std::move(signature_sets)));
  requests.front() = Value(std::move(encoded_request));
  auto packet = valid;
  packet.insert_or_assign("requests", Value(std::move(requests)));
  expect_protocol_error(decode_isolated_message(packet_json(std::move(packet))),
                        "$.requests[0].assetSignatures[0][0].fileSize");
  requests = valid.at("requests").as_array();
  encoded_request = requests.front().as_object();
  encoded_request.insert_or_assign(
      "assetSignatures", Value(Value::Array{Value(Value::Array{})}));
  requests.front() = Value(std::move(encoded_request));
  packet = valid;
  packet.insert_or_assign("requests", Value(std::move(requests)));
  expect_protocol_error(decode_isolated_message(packet_json(std::move(packet))),
                        "asset signatures must match template references");
  packet = valid;
  packet.insert_or_assign(
      "exclusions", Value(Value::Array{Value(Value::Array{Value(1.0)})}));
  expect_protocol_error(decode_isolated_message(packet_json(std::move(packet))),
                        "$.exclusions[0]: exclusion must contain four coordinates");
  packet = valid;
  packet.insert_or_assign("requests", Value(Value::Array(65)));
  expect_protocol_error(decode_isolated_message(packet_json(std::move(packet))),
                        "requests must be an array of at most 64");
  packet = valid;
  packet.insert_or_assign("exclusions", Value(Value::Array(129)));
  expect_protocol_error(decode_isolated_message(packet_json(std::move(packet))),
                        "exclusions must be an array of at most 128");
  expect(std::holds_alternative<IsolatedConfiguration>(
             decode_isolated_message(packet_json(valid))),
         "a nested decode failure must not retain partial state across messages");
}

void nested_snapshot_errors_preserve_detail_and_bounds() {
  using namespace layerc::daemon;
  const auto now = layerc::core::MonotonicClock::now();
  const auto valid = packet_object(encode_isolated_snapshot(7, snapshot(1, 1, now)));
  auto evaluations = valid.at("evaluations").as_array();
  auto encoded_evaluation = evaluations.front().as_object();
  auto measurements = encoded_evaluation.at("measurements").as_array();
  auto measurement = measurements.front().as_object();
  measurement.insert_or_assign("confidence", Value(2.0));
  measurements.front() = Value(std::move(measurement));
  encoded_evaluation.insert_or_assign("measurements", Value(std::move(measurements)));
  evaluations.front() = Value(std::move(encoded_evaluation));
  auto packet = valid;
  packet.insert_or_assign("evaluations", Value(std::move(evaluations)));
  expect_protocol_error(decode_isolated_snapshot(packet_json(std::move(packet)), 7),
                        "$.evaluations[0].measurements[0].confidence");
  evaluations = valid.at("evaluations").as_array();
  encoded_evaluation = evaluations.front().as_object();
  encoded_evaluation.insert_or_assign("measurements", Value(Value::Array(9)));
  evaluations.front() = Value(std::move(encoded_evaluation));
  packet = valid;
  packet.insert_or_assign("evaluations", Value(std::move(evaluations)));
  expect_protocol_error(decode_isolated_snapshot(packet_json(std::move(packet)), 7),
                        "invalid isolated measurements");
  packet = valid;
  packet.insert_or_assign("evaluations", Value(Value::Array(65)));
  expect_protocol_error(decode_isolated_snapshot(packet_json(std::move(packet)), 7),
                        "invalid isolated evaluations");
  expect(std::holds_alternative<VisualCaptureSnapshot>(
             decode_isolated_snapshot(packet_json(valid), 7)),
         "a nested decode failure must not poison the next valid snapshot");
}

void rejected_snapshots_do_not_advance_connection_state() {
  using namespace layerc::daemon;
  const auto now = layerc::core::MonotonicClock::now();
  IsolatedSnapshotValidator validator;
  validator.begin_connection();
  auto first = snapshot(1, 1, now);
  expect(verdict(validator, first, now) == IsolatedSnapshotDisposition::accepted,
         "a fresh connection should accept its first frame");
  auto future = snapshot(2, 2, now + std::chrono::milliseconds(251));
  expect(verdict(validator, future, now) == std::nullopt,
         "future-dated samples must not advance the connection");
  auto valid = snapshot(2, 2, now);
  expect(verdict(validator, valid, now) ==
                 IsolatedSnapshotDisposition::accepted &&
             valid.sequence == 2 && valid.evaluations.front().sequence == 2,
         "a valid retry of the same sequence should recover after rejection");
  auto partial = snapshot(3, 3, now);
  partial.evaluations.push_back(evaluation("late.json", 1, {}));
  expect(verdict(validator, partial, now) ==
             std::nullopt,
         "one invalid evaluation must reject the whole snapshot");
  auto recovered = snapshot(3, 3, now);
  expect(verdict(validator, recovered, now) ==
                 IsolatedSnapshotDisposition::accepted &&
             recovered.sequence == 3 && recovered.evaluations.front().sequence == 3,
         "a partial rejection must not mutate any evaluation's sequence");
  auto backwards = snapshot(4, 2, now);
  expect(verdict(validator, backwards, now) ==
             std::nullopt,
         "an evaluation sequence must not move backwards within a connection");
}

} // namespace

int main() {
  return layerc::test::run({
      {"handshake_is_mutually_bound", handshake_is_mutually_bound},
      {"configuration_and_packet_sequences_round_trip", configuration_and_packet_sequences_round_trip},
      {"inbound_messages_decode_in_one_pass", inbound_messages_decode_in_one_pass},
      {"worker_sequences_survive_quality_epochs", worker_sequences_survive_quality_epochs},
      {"snapshots_reject_replay_and_staleness", snapshots_reject_replay_and_staleness},
      {"replacement_helpers_start_new_epochs", replacement_helpers_start_new_epochs},
      {"cached_evaluations_are_not_republished_as_fresh", cached_evaluations_are_not_republished_as_fresh},
      LAYERC_TEST_CASE(decoder_errors_are_single_typed_results),
      LAYERC_TEST_CASE(nested_configuration_errors_preserve_detail_and_bounds),
      LAYERC_TEST_CASE(nested_snapshot_errors_preserve_detail_and_bounds),
      LAYERC_TEST_CASE(rejected_snapshots_do_not_advance_connection_state),
      LAYERC_TEST_CASE(snapshot_capture_health_round_trips),
      LAYERC_TEST_CASE(capture_faults_do_not_fault_unrelated_services),
      LAYERC_TEST_CASE(inline_capture_status_preserves_fault_reason),
  });
}
