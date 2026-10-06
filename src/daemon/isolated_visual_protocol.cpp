#include "isolated_visual_protocol.hpp"

#include "layerc/core/json.hpp"
#include "layerc/core/json_reader.hpp"
#include "layerc/core/visual.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace layerc::daemon {
namespace {

using layerc::core::json::EmptyString;
using layerc::core::json::IntegerEncoding;
using layerc::core::json::Reader;
using layerc::core::json::Required;
using layerc::core::json::Value;

// Counters and identifiers travel as JSON integer tokens (full int64 range).
inline constexpr auto wire_integer = IntegerEncoding::integer_token;

std::optional<std::uint64_t> wire_unsigned(
    Reader &reader, const Value &parent, const std::string_view key,
    const std::string &path,
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max()) {
  return reader.unsigned_integer(parent, key, path, maximum, Required::yes, 0,
                                 wire_integer);
}

IsolatedProtocolError reader_error(const Reader &reader) {
  const auto described = reader.error().describe();
  return {described.empty() ? std::string{"invalid isolated visual message"}
                           : described};
}

IsolatedProtocolResult<Value> object_root(const std::string_view source) {
  auto parsed = layerc::core::json::parse_bounded(
      source, isolated_visual_max_message_bytes);
  if (const auto *parse_error =
          std::get_if<layerc::core::json::FieldError>(&parsed))
    return IsolatedProtocolError{parse_error->describe()};
  auto root = std::move(std::get<Value>(parsed));
  if (!root.is_object())
    return IsolatedProtocolError{
        "$: isolated visual message root must be an object"};
  Reader reader;
  const auto version =
      reader.integer(root, "protocolVersion", "$",
                     isolated_visual_protocol_version,
                     isolated_visual_protocol_version, Required::yes);
  if (!version)
    return reader_error(reader);
  std::string package;
  if (!reader.string(root, "packageVersion", "$", package, Required::yes,
                     EmptyString::reject))
    return reader_error(reader);
  if (package != layerc::core::product_version)
    return IsolatedProtocolError{
        "$.packageVersion: isolated visual package version mismatch"};
  return root;
}

Value wire_number(const std::uint64_t value) {
  // The decoder accepts int64 tokens only; never narrow or quote a counter.
  if (value > static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max()))
    throw std::out_of_range("isolated visual integer exceeds int64 wire range");
  return Value(static_cast<std::int64_t>(value));
}

Value::Object packet_object(const std::string_view type) {
  return {{"protocolVersion", wire_number(isolated_visual_protocol_version)},
          {"packageVersion", Value(std::string(layerc::core::product_version))},
          {"type", Value(std::string(type))}};
}

std::string encode_packet(Value::Object packet) {
  std::string output;
  layerc::core::json::append_value(output, Value(std::move(packet)));
  return output;
}

std::string encode_sequence_packet(const std::string_view type,
                                   const std::uint64_t sequence) {
  auto packet = packet_object(type);
  packet.emplace("sequence", wire_number(sequence));
  return encode_packet(std::move(packet));
}

std::uint64_t monotonic_nanoseconds(const layerc::core::TimePoint value) {
  const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         value.time_since_epoch())
                         .count();
  return count < 0 ? 0U : static_cast<std::uint64_t>(count);
}

Value visual_profile_value(const layerc::core::VisualProfile &visual) {
  // Reuse the core serializer rather than duplicating the profile schema.
  return std::get<Value>(layerc::core::json::parse(
      layerc::core::serialize_visual_profile_json(visual)));
}

IsolatedProtocolResult<VisualAssetSignature>
decode_asset_signature(const Value &encoded, const std::string &item_path) {
  if (!encoded.is_object())
    return IsolatedProtocolError{item_path + ": asset signature must be an object"};
  Reader reader;
  VisualAssetSignature signature;
  if (!reader.boolean(encoded, "present", item_path, signature.present,
                      Required::yes))
    return reader_error(reader);
  const auto volume_serial =
      wire_unsigned(reader, encoded, "volumeSerial", item_path,
                    std::numeric_limits<std::uint32_t>::max());
  const auto file_index = wire_unsigned(reader, encoded, "fileIndex", item_path);
  const auto file_size = wire_unsigned(reader, encoded, "fileSize", item_path);
  const auto write_ticks =
      wire_unsigned(reader, encoded, "lastWriteTicks", item_path);
  const auto change_usn = wire_unsigned(reader, encoded, "changeUsn", item_path);
  if (!volume_serial || !file_index || !file_size || !write_ticks || !change_usn)
    return reader_error(reader);
  signature.volume_serial = static_cast<std::uint32_t>(*volume_serial);
  signature.file_index = *file_index;
  signature.file_size = *file_size;
  signature.last_write_ticks = *write_ticks;
  signature.change_usn = *change_usn;
  return signature;
}

IsolatedProtocolResult<VisualTemplateEvaluation>
decode_template_evaluation(const Value &encoded_signatures,
                           layerc::core::TemplateState state,
                           const std::string &signature_path) {
  if (!encoded_signatures.is_array())
    return IsolatedProtocolError{
        signature_path + ": expected an array of signatures"};
  const auto &signature_list = encoded_signatures.as_array();
  if (signature_list.size() != state.references.size())
    return IsolatedProtocolError{"asset signatures must match template references"};
  VisualTemplateEvaluation evaluation;
  evaluation.state = std::move(state);
  evaluation.asset_signatures.reserve(signature_list.size());
  for (std::size_t index = 0; index < signature_list.size(); ++index) {
    const auto item_path = signature_path + "[" + std::to_string(index) + "]";
    auto decoded = decode_asset_signature(signature_list[index], item_path);
    if (const auto *error = std::get_if<IsolatedProtocolError>(&decoded))
      return *error;
    evaluation.asset_signatures.push_back(
        std::get<VisualAssetSignature>(std::move(decoded)));
  }
  return evaluation;
}

IsolatedProtocolResult<VisualEvaluationRequest>
decode_visual_request(const Value &entry, const std::string &path) {
  if (!entry.is_object())
    return IsolatedProtocolError{path + ": request must be an object"};
  Reader reader;
  std::string source_key;
  if (!reader.string(entry, "sourceKey", path, source_key, Required::yes,
                     EmptyString::reject))
    return reader_error(reader);
  const auto *visual_value =
      reader.object(entry, "visualProfile", path, Required::yes);
  if (visual_value == nullptr)
    return reader_error(reader);
  layerc::core::GameProfile base;
  base.id = "ipc";
  const auto decoded =
      layerc::core::parse_visual_profile_value(*visual_value, base);
  const auto *visual = std::get_if<layerc::core::VisualProfile>(&decoded);
  if (visual == nullptr)
    return IsolatedProtocolError{"invalid visual evaluation request"};
  VisualEvaluationRequest request;
  request.source_key = std::move(source_key);
  request.sample_rate_hz = visual->sample_rate_hz;
  request.quality = visual->quality;
  for (const auto &rule :
       layerc::core::observations_of<layerc::core::PixelRule>(
           visual->observations))
    request.rules.push_back(rule);
  std::vector<const layerc::core::TemplateState *> templates;
  for (const auto &state :
       layerc::core::observations_of<layerc::core::TemplateState>(
           visual->observations))
    templates.push_back(&state);
  const auto *signature_sets =
      reader.array(entry, "assetSignatures", path, Required::yes);
  if (signature_sets == nullptr)
    return reader_error(reader);
  if (signature_sets->size() != templates.size())
    return IsolatedProtocolError{
        "assetSignatures must match the visual template count"};
  request.templates.reserve(templates.size());
  for (std::size_t index = 0; index < templates.size(); ++index) {
    const auto signature_path =
        path + ".assetSignatures[" + std::to_string(index) + "]";
    auto evaluation = decode_template_evaluation(
        (*signature_sets)[index], *templates[index], signature_path);
    if (const auto *error = std::get_if<IsolatedProtocolError>(&evaluation))
      return *error;
    request.templates.push_back(
        std::get<VisualTemplateEvaluation>(std::move(evaluation)));
  }
  return request;
}

IsolatedProtocolResult<RECT> decode_exclusion_rect(const Value &entry,
                                                 const std::string &path) {
  if (!entry.is_array() || entry.as_array().size() != 4)
    return IsolatedProtocolError{
        path + ": exclusion must contain four coordinates"};
  RECT rect{};
  long *coordinates[] = {&rect.left, &rect.top, &rect.right, &rect.bottom};
  for (std::size_t coordinate = 0; coordinate < 4; ++coordinate) {
    const auto &value = entry.as_array()[coordinate];
    if (!value.is_number() || !std::isfinite(value.as_number()) ||
        value.as_number() < std::numeric_limits<long>::min() ||
        value.as_number() > std::numeric_limits<long>::max())
      return IsolatedProtocolError{path + ": invalid exclusion coordinate"};
    *coordinates[coordinate] = static_cast<long>(value.as_number());
  }
  return rect;
}

IsolatedInboundMessage decode_configure_message(const Value &root) {
  Reader reader;
  IsolatedConfiguration result;
  const auto sequence = wire_unsigned(reader, root, "sequence", "$");
  const auto quality = reader.integer(
      root, "captureQuality", "$", 0, layerc::core::capture_quality_max,
      Required::yes);
  const auto *requests = reader.array(root, "requests", "$", Required::yes);
  if (!sequence || !quality || requests == nullptr)
    return reader_error(reader);
  result.sequence = *sequence;
  result.capture_quality = static_cast<layerc::core::CaptureQuality>(*quality);
  if (requests->size() > 64)
    return IsolatedProtocolError{"requests must be an array of at most 64"};
  result.requests.reserve(requests->size());
  for (std::size_t index = 0; index < requests->size(); ++index) {
    const auto path = "$.requests[" + std::to_string(index) + "]";
    auto request = decode_visual_request((*requests)[index], path);
    if (const auto *error = std::get_if<IsolatedProtocolError>(&request))
      return *error;
    result.requests.push_back(
        std::get<VisualEvaluationRequest>(std::move(request)));
  }
  const auto *exclusions = reader.array(root, "exclusions", "$", Required::yes);
  if (exclusions == nullptr)
    return reader_error(reader);
  if (exclusions->size() > 128)
    return IsolatedProtocolError{"exclusions must be an array of at most 128"};
  for (std::size_t index = 0; index < exclusions->size(); ++index) {
    const auto path = "$.exclusions[" + std::to_string(index) + "]";
    auto decoded = decode_exclusion_rect((*exclusions)[index], path);
    if (const auto *error = std::get_if<IsolatedProtocolError>(&decoded))
      return *error;
    const auto &rect = std::get<RECT>(decoded);
    if (rect.right > rect.left && rect.bottom > rect.top)
      result.exclusions.push_back(rect);
  }
  return result;
}

IsolatedInboundMessage decode_poll_message(const Value &root) {
  Reader reader;
  const auto sequence = wire_unsigned(reader, root, "sequence", "$");
  if (!sequence)
    return reader_error(reader);
  return IsolatedPoll{*sequence};
}

Value asset_signature_value(const VisualAssetSignature &signature) {
  return Value(Value::Object{
      {"present", Value(signature.present)},
      {"volumeSerial", wire_number(signature.volume_serial)},
      {"fileIndex", wire_number(signature.file_index)},
      {"fileSize", wire_number(signature.file_size)},
      {"lastWriteTicks", wire_number(signature.last_write_ticks)},
      {"changeUsn", wire_number(signature.change_usn)}});
}

Value visual_request_value(const VisualEvaluationRequest &request) {
  layerc::core::VisualProfile visual;
  visual.profile_id = "ipc";
  visual.sample_rate_hz = request.sample_rate_hz;
  visual.quality = request.quality;
  visual.observations.reserve(request.rules.size() + request.templates.size());
  for (const auto &rule : request.rules)
    visual.observations.emplace_back(rule);
  Value::Array signature_sets;
  for (const auto &evaluation : request.templates) {
    visual.observations.emplace_back(evaluation.state);
    Value::Array signatures;
    for (const auto &signature : evaluation.asset_signatures)
      signatures.push_back(asset_signature_value(signature));
    signature_sets.emplace_back(std::move(signatures));
  }
  return Value(Value::Object{
      {"sourceKey", Value(request.source_key)},
      {"visualProfile", visual_profile_value(visual)},
      {"assetSignatures", Value(std::move(signature_sets))}});
}

Value measurement_value(
    const layerc::core::ObservationMeasurement &measurement) {
  return Value(Value::Object{
      {"id", Value(measurement.observation_id)},
      {"availability",
       wire_number(static_cast<std::uint8_t>(measurement.availability))},
      {"matched", Value(measurement.matched)},
      {"confidence", Value(measurement.confidence)},
      {"sampledAtNs", wire_number(monotonic_nanoseconds(measurement.sampled_at))}});
}

Value evaluation_value(const VisualEvaluationSnapshot &evaluation) {
  Value::Array measurements;
  for (const auto &measurement : evaluation.measurements)
    measurements.push_back(measurement_value(measurement));
  return Value(Value::Object{
      {"sourceKey", Value(evaluation.source_key)},
      {"sequence", wire_number(evaluation.sequence)},
      {"missingAssets", Value(evaluation.missing_template_assets)},
      {"measurements", Value(std::move(measurements))}});
}

IsolatedProtocolResult<layerc::core::ObservationMeasurement>
decode_measurement(const Value &encoded, const std::string &path) {
  if (!encoded.is_object())
    return IsolatedProtocolError{path + ": measurement must be an object"};
  Reader reader;
  layerc::core::ObservationMeasurement measurement;
  bool matched{};
  if (!reader.string(encoded, "id", path, measurement.observation_id,
                     Required::yes, EmptyString::reject) ||
      !reader.boolean(encoded, "matched", path, matched, Required::yes))
    return reader_error(reader);
  const auto sampled_at = wire_unsigned(reader, encoded, "sampledAtNs", path);
  const auto confidence =
      reader.number(encoded, "confidence", path, 0.0, 1.0, Required::yes);
  const auto availability = reader.integer(
      encoded, "availability", path, 0, layerc::core::capture_availability_max,
      Required::yes);
  if (!sampled_at || !confidence || !availability)
    return reader_error(reader);
  measurement.availability =
      static_cast<layerc::core::CaptureAvailability>(*availability);
  measurement.matched = matched;
  measurement.confidence = *confidence;
  measurement.sampled_at =
      layerc::core::TimePoint(std::chrono::nanoseconds(*sampled_at));
  return measurement;
}

IsolatedProtocolResult<VisualEvaluationSnapshot>
decode_evaluation(const Value &entry, const std::string &path) {
  if (!entry.is_object())
    return IsolatedProtocolError{path + ": isolated evaluation must be an object"};
  Reader reader;
  std::string source_key;
  bool missing{};
  if (!reader.string(entry, "sourceKey", path, source_key, Required::yes,
                     EmptyString::reject) ||
      !reader.boolean(entry, "missingAssets", path, missing, Required::yes))
    return reader_error(reader);
  const auto sequence = wire_unsigned(reader, entry, "sequence", path);
  const auto *measurements =
      reader.array(entry, "measurements", path, Required::yes);
  if (!sequence || measurements == nullptr)
    return reader_error(reader);
  if (measurements->size() > 8)
    return IsolatedProtocolError{"invalid isolated measurements"};
  VisualEvaluationSnapshot evaluation;
  evaluation.source_key = std::move(source_key);
  evaluation.sequence = *sequence;
  evaluation.missing_template_assets = missing;
  for (std::size_t index = 0; index < measurements->size(); ++index) {
    const auto measurement_path =
        path + ".measurements[" + std::to_string(index) + "]";
    auto measurement = decode_measurement((*measurements)[index], measurement_path);
    if (const auto *error = std::get_if<IsolatedProtocolError>(&measurement))
      return *error;
    evaluation.measurements.push_back(
        std::get<layerc::core::ObservationMeasurement>(std::move(measurement)));
  }
  return evaluation;
}

} // namespace

void IsolatedSnapshotRebaser::begin_worker_epoch() noexcept {
  local_capture_sequence_ = 0;
  for (auto &[key, evaluation] : evaluations_) {
    (void)key;
    evaluation.local = 0;
  }
}

bool IsolatedSnapshotRebaser::rebase(VisualCaptureSnapshot &snapshot) {
  const bool advanced =
      snapshot.sequence != 0 && snapshot.sequence != local_capture_sequence_;
  if (advanced) {
    local_capture_sequence_ = snapshot.sequence;
    ++capture_sequence_;
  }
  snapshot.sequence = capture_sequence_;
  for (auto &evaluation : snapshot.evaluations) {
    // A profile sampled slower than the shared capture keeps its previous
    // local sequence and is republished unchanged, not as a fresh sample.
    auto &state = evaluations_[evaluation.source_key];
    if (evaluation.sequence != 0 && evaluation.sequence != state.local) {
      state.local = evaluation.sequence;
      ++state.published;
    }
    evaluation.sequence = state.published;
  }
  return advanced;
}

void IsolatedSnapshotValidator::begin_connection() noexcept {
  local_capture_sequence_ = 0;
  capture_base_ = capture_sequence_;
  for (auto &[key, evaluation] : evaluations_) {
    (void)key;
    evaluation.local = 0;
    evaluation.base = evaluation.published;
  }
}

IsolatedProtocolResult<IsolatedSnapshotDisposition>
IsolatedSnapshotValidator::validate(VisualCaptureSnapshot &snapshot,
                                    const layerc::core::TimePoint now) {
  // Checks use this connection's local sequences; accepted values are then
  // mapped onto the host stream above everything earlier helpers published.
  if (snapshot.sequence < local_capture_sequence_) {
    return IsolatedProtocolError{"isolated capture sequence moved backwards"};
  }
  constexpr auto maximum_age = std::chrono::seconds(2);
  constexpr auto maximum_future_skew = std::chrono::milliseconds(250);
  for (const auto &evaluation : snapshot.evaluations) {
    const auto previous = evaluations_.find(evaluation.source_key);
    if (previous != evaluations_.end() &&
        evaluation.sequence < previous->second.local) {
      return IsolatedProtocolError{"isolated evaluation sequence moved backwards"};
    }
    for (const auto &measurement : evaluation.measurements) {
      if (measurement.sampled_at == layerc::core::TimePoint{} ||
          measurement.sampled_at > now + maximum_future_skew ||
          now - measurement.sampled_at > maximum_age) {
        return IsolatedProtocolError{"isolated measurement timestamp is stale or invalid"};
      }
    }
  }
  if (snapshot.sequence == local_capture_sequence_)
    return IsolatedSnapshotDisposition::duplicate;

  std::erase_if(snapshot.evaluations, [&](const auto &evaluation) {
    const auto previous = evaluations_.find(evaluation.source_key);
    return previous != evaluations_.end() &&
           evaluation.sequence == previous->second.local;
  });
  local_capture_sequence_ = snapshot.sequence;
  capture_sequence_ = capture_base_ + snapshot.sequence;
  snapshot.sequence = capture_sequence_;
  for (auto &evaluation : snapshot.evaluations) {
    auto &state = evaluations_[evaluation.source_key];
    state.local = evaluation.sequence;
    if (evaluation.sequence != 0) {
      state.published = state.base + evaluation.sequence;
      evaluation.sequence = state.published;
    }
  }
  return IsolatedSnapshotDisposition::accepted;
}

std::string isolated_hello_json(const std::string_view nonce,
                               const std::string_view role,
                               const std::uint32_t process_id) {
  auto packet = packet_object("hello");
  packet.emplace("nonce", Value(std::string(nonce)));
  packet.emplace("role", Value(std::string(role)));
  packet.emplace("processId", wire_number(process_id));
  return encode_packet(std::move(packet));
}

IsolatedStatusResult validate_isolated_hello(
    const std::string_view json, const std::string_view nonce,
    const std::string_view expected_role, const std::uint32_t expected_process_id) {
  auto parsed = object_root(json);
  if (const auto *error = std::get_if<IsolatedProtocolError>(&parsed))
    return *error;
  const auto &root = std::get<Value>(parsed);
  Reader reader;
  std::string type, actual_nonce, role;
  const auto hello_fields_ok =
      reader.string(root, "type", "$", type, Required::yes, EmptyString::reject) &&
      reader.string(root, "nonce", "$", actual_nonce, Required::yes,
                    EmptyString::allow) &&
      reader.string(root, "role", "$", role, Required::yes, EmptyString::reject);
  const auto process_id = wire_unsigned(
      reader, root, "processId", "$", std::numeric_limits<std::uint32_t>::max());
  if (!hello_fields_ok || !process_id)
    return reader_error(reader);
  if (type != "hello")
    return IsolatedProtocolError{"$.type: expected hello"};
  if (actual_nonce != nonce || role != expected_role ||
      *process_id != expected_process_id)
    return IsolatedProtocolError{"isolated visual hello identity mismatch"};
  return std::monostate{};
}

std::string
encode_isolated_configuration(const IsolatedConfiguration &configuration) {
  auto packet = packet_object("configure");
  packet.emplace("sequence", wire_number(configuration.sequence));
  packet.emplace(
      "captureQuality",
      wire_number(static_cast<std::uint8_t>(configuration.capture_quality)));
  Value::Array requests;
  for (const auto &request : configuration.requests)
    requests.push_back(visual_request_value(request));
  packet.emplace("requests", Value(std::move(requests)));
  Value::Array exclusions;
  for (const auto &rect : configuration.exclusions) {
    exclusions.emplace_back(Value::Array{
        Value(static_cast<double>(rect.left)),
        Value(static_cast<double>(rect.top)),
        Value(static_cast<double>(rect.right)),
        Value(static_cast<double>(rect.bottom))});
  }
  packet.emplace("exclusions", Value(std::move(exclusions)));
  return encode_packet(std::move(packet));
}

IsolatedInboundMessage decode_isolated_message(const std::string_view json) {
  auto parsed = object_root(json);
  if (const auto *error = std::get_if<IsolatedProtocolError>(&parsed))
    return *error;
  const auto &root = std::get<Value>(parsed);
  Reader reader;
  const auto type =
      reader.string(root, "type", "$", Required::yes, EmptyString::reject);
  if (!type)
    return reader_error(reader);
  if (*type == "configure")
    return decode_configure_message(root);
  if (*type == "waitSnapshot")
    return decode_poll_message(root);
  return IsolatedProtocolError{"unexpected isolated visual message type"};
}

std::string encode_isolated_poll(const std::uint64_t sequence) {
  return encode_sequence_packet("waitSnapshot", sequence);
}

std::string encode_isolated_snapshot(const std::uint64_t packet_sequence,
                                    const VisualCaptureSnapshot &snapshot) {
  auto packet = packet_object("snapshot");
  packet.emplace("sequence", wire_number(packet_sequence));
  packet.emplace("captureSequence", wire_number(snapshot.sequence));
  packet.emplace(
      "availability",
      wire_number(static_cast<std::uint8_t>(snapshot.result.availability)));
  packet.emplace("backend",
                 wire_number(static_cast<std::uint8_t>(snapshot.result.backend)));
  packet.emplace("diagnostic", Value(snapshot.result.diagnostic));
  packet.emplace("gpu", Value(snapshot.gpu_description));
  Value::Array evaluations;
  for (const auto &evaluation : snapshot.evaluations)
    evaluations.push_back(evaluation_value(evaluation));
  packet.emplace("evaluations", Value(std::move(evaluations)));
  return encode_packet(std::move(packet));
}

IsolatedSnapshotResult
decode_isolated_snapshot(const std::string_view json,
                         const std::uint64_t expected_packet_sequence) {
  auto parsed = object_root(json);
  if (const auto *error = std::get_if<IsolatedProtocolError>(&parsed))
    return *error;
  const auto &root = std::get<Value>(parsed);
  Reader reader;
  std::string type, gpu, diagnostic;
  if (!reader.string(root, "type", "$", type, Required::yes, EmptyString::reject) ||
      !reader.string(root, "gpu", "$", gpu, Required::yes, EmptyString::allow) ||
      !reader.string(root, "diagnostic", "$", diagnostic, Required::yes,
                     EmptyString::allow))
    return reader_error(reader);
  const auto packet_sequence = wire_unsigned(reader, root, "sequence", "$");
  const auto capture_sequence =
      wire_unsigned(reader, root, "captureSequence", "$");
  const auto availability = reader.integer(
      root, "availability", "$", 0, layerc::core::capture_availability_max,
      Required::yes);
  const auto backend = reader.integer(
      root, "backend", "$", 0, isolated_capture_backend_max, Required::yes);
  const auto *evaluations = reader.array(root, "evaluations", "$", Required::yes);
  if (!packet_sequence || !capture_sequence || !availability || !backend ||
      evaluations == nullptr)
    return reader_error(reader);
  if (type != "snapshot")
    return IsolatedProtocolError{"invalid isolated snapshot"};
  if (*packet_sequence != expected_packet_sequence)
    return IsolatedProtocolError{"out-of-order isolated snapshot"};
  if (evaluations->size() > 64)
    return IsolatedProtocolError{"invalid isolated evaluations"};
  VisualCaptureSnapshot result;
  result.sequence = *capture_sequence;
  result.result.availability =
      static_cast<layerc::core::CaptureAvailability>(*availability);
  result.result.backend =
      static_cast<layerc::core::CaptureBackendKind>(*backend);
  result.result.diagnostic = std::move(diagnostic);
  result.gpu_description = std::move(gpu);
  for (std::size_t index = 0; index < evaluations->size(); ++index) {
    const auto path = "$.evaluations[" + std::to_string(index) + "]";
    auto evaluation = decode_evaluation((*evaluations)[index], path);
    if (const auto *error = std::get_if<IsolatedProtocolError>(&evaluation))
      return *error;
    result.evaluations.push_back(
        std::get<VisualEvaluationSnapshot>(std::move(evaluation)));
  }
  return result;
}

std::string encode_isolated_ack(const std::uint64_t sequence) {
  return encode_sequence_packet("ack", sequence);
}

IsolatedStatusResult decode_isolated_ack(const std::string_view json,
                                        const std::uint64_t expected_sequence) {
  auto parsed = object_root(json);
  if (const auto *error = std::get_if<IsolatedProtocolError>(&parsed))
    return *error;
  const auto &root = std::get<Value>(parsed);
  Reader reader;
  std::string type;
  const auto type_ok = reader.string(root, "type", "$", type, Required::yes,
                                     EmptyString::reject);
  const auto sequence = wire_unsigned(reader, root, "sequence", "$");
  if (!type_ok || !sequence)
    return reader_error(reader);
  if (type != "ack" || *sequence != expected_sequence)
    return IsolatedProtocolError{"invalid isolated acknowledgement"};
  return std::monostate{};
}

} // namespace layerc::daemon
