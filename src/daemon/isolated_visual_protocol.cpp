#include "isolated_visual_protocol.hpp"

#include "layerc/core/json.hpp"
#include "layerc/core/json_reader.hpp"
#include "layerc/core/visual.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>

namespace layerc::daemon {
namespace {

using layerc::core::json::EmptyString;
using layerc::core::json::IntegerEncoding;
using layerc::core::json::Reader;
using layerc::core::json::Required;
using layerc::core::json::Value;

// Counters and identifiers travel as JSON integer tokens (full int64 range).
inline constexpr auto wire_integer = IntegerEncoding::integer_token;

// Required counter/identifier field, read as a wire integer token.
std::optional<std::uint64_t> wire_unsigned(
    Reader &reader, const Value &parent, const std::string_view key,
    const std::string &path,
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max()) {
  return reader.unsigned_integer(parent, key, path, maximum, Required::yes, 0,
                                 wire_integer);
}

std::string reader_error(const Reader &reader) {
  const auto described = reader.error().describe();
  return described.empty() ? std::string{"invalid isolated visual message"}
                           : described;
}

const Value *object_root(const std::string_view source, Reader &reader,
                         layerc::core::json::ParseResult &storage) {
  if (source.size() > isolated_visual_max_message_bytes) {
    reader.fail("$", "isolated visual message exceeds 1 MiB");
    return nullptr;
  }
  storage = layerc::core::json::parse(source);
  if (const auto *parse_error =
          std::get_if<layerc::core::json::ParseError>(&storage)) {
    reader.fail("$", "invalid isolated visual JSON at byte " +
                         std::to_string(parse_error->offset) + ": " +
                         parse_error->message);
    return nullptr;
  }
  const auto &root = std::get<Value>(storage);
  if (!root.is_object()) {
    reader.fail("$", "isolated visual message root must be an object");
    return nullptr;
  }
  const auto version =
      reader.integer(root, "protocolVersion", "$",
                     isolated_visual_protocol_version,
                     isolated_visual_protocol_version, Required::yes);
  if (!version)
    return nullptr;
  std::string package;
  if (!reader.string(root, "packageVersion", "$", package, Required::yes,
                     EmptyString::reject))
    return nullptr;
  if (package != layerc::core::product_version) {
    reader.fail("$.packageVersion", "isolated visual package version mismatch");
    return nullptr;
  }
  return &root;
}

void append_packet_prefix(std::string &output, const std::string_view type) {
  output += "{\"protocolVersion\":";
  layerc::core::json::append_unsigned(output, isolated_visual_protocol_version);
  output += ",\"packageVersion\":";
  layerc::core::json::append_quoted(output, layerc::core::product_version);
  output += ",\"type\":";
  layerc::core::json::append_quoted(output, type);
}

std::string encode_sequence_packet(const std::string_view type,
                                   const std::uint64_t sequence) {
  std::string output;
  append_packet_prefix(output, type);
  output += ",\"sequence\":";
  layerc::core::json::append_unsigned(output, sequence);
  output.push_back('}');
  return output;
}

std::uint64_t monotonic_nanoseconds(const layerc::core::TimePoint value) {
  const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         value.time_since_epoch())
                         .count();
  return count < 0 ? 0U : static_cast<std::uint64_t>(count);
}

void append_visual_profile_object(std::string &output,
                                  const layerc::core::VisualProfile &visual) {
  auto encoded = layerc::core::serialize_visual_profile_json(visual);
  while (!encoded.empty() &&
         (encoded.back() == '\n' || encoded.back() == '\r'))
    encoded.pop_back();
  output += encoded;
}

std::optional<VisualAssetSignature>
decode_asset_signature(const Value &encoded, Reader &reader,
                       const std::string &item_path, std::string &error) {
  if (!encoded.is_object()) {
    error = item_path + ": asset signature must be an object";
    return std::nullopt;
  }
  VisualAssetSignature signature;
  if (!reader.boolean(encoded, "present", item_path, signature.present,
                      Required::yes)) {
    error = reader_error(reader);
    return std::nullopt;
  }
  const auto volume_serial =
      wire_unsigned(reader, encoded, "volumeSerial", item_path,
                    std::numeric_limits<std::uint32_t>::max());
  const auto file_index =
      wire_unsigned(reader, encoded, "fileIndex", item_path);
  const auto file_size = wire_unsigned(reader, encoded, "fileSize", item_path);
  const auto write_ticks =
      wire_unsigned(reader, encoded, "lastWriteTicks", item_path);
  const auto change_usn =
      wire_unsigned(reader, encoded, "changeUsn", item_path);
  if (!volume_serial || !file_index || !file_size || !write_ticks ||
      !change_usn) {
    error = reader_error(reader);
    return std::nullopt;
  }
  signature.volume_serial = static_cast<std::uint32_t>(*volume_serial);
  signature.file_index = *file_index;
  signature.file_size = *file_size;
  signature.last_write_ticks = *write_ticks;
  signature.change_usn = *change_usn;
  return signature;
}

std::optional<VisualTemplateEvaluation>
decode_template_evaluation(const Value &encoded_signatures,
                           layerc::core::TemplateState state, Reader &reader,
                           const std::string &signature_path,
                           std::string &error) {
  if (!encoded_signatures.is_array()) {
    error = signature_path + ": expected an array of signatures";
    return std::nullopt;
  }
  const auto &signature_list = encoded_signatures.as_array();
  if (signature_list.size() != state.references.size()) {
    error = "asset signatures must match template references";
    return std::nullopt;
  }
  VisualTemplateEvaluation evaluation;
  evaluation.state = std::move(state);
  evaluation.asset_signatures.reserve(signature_list.size());
  for (std::size_t signature_index = 0; signature_index < signature_list.size();
       ++signature_index) {
    const auto item_path =
        signature_path + "[" + std::to_string(signature_index) + "]";
    auto signature = decode_asset_signature(signature_list[signature_index],
                                            reader, item_path, error);
    if (!signature)
      return std::nullopt;
    evaluation.asset_signatures.push_back(*signature);
  }
  return evaluation;
}

std::optional<VisualEvaluationRequest>
decode_visual_request(const Value &entry, Reader &reader,
                      const std::string &path, std::string &error) {
  if (!entry.is_object()) {
    error = path + ": request must be an object";
    return std::nullopt;
  }
  std::string source_key;
  if (!reader.string(entry, "sourceKey", path, source_key, Required::yes,
                     EmptyString::reject)) {
    error = reader_error(reader);
    return std::nullopt;
  }
  const auto *visual_value =
      reader.object(entry, "visualProfile", path, Required::yes);
  if (visual_value == nullptr) {
    error = reader_error(reader);
    return std::nullopt;
  }
  layerc::core::GameProfile base;
  base.id = "ipc";
  const auto decoded =
      layerc::core::parse_visual_profile_value(*visual_value, base);
  const auto *visual = std::get_if<layerc::core::VisualProfile>(&decoded);
  if (visual == nullptr) {
    error = "invalid visual evaluation request";
    return std::nullopt;
  }
  VisualEvaluationRequest request;
  request.source_key = std::move(source_key);
  request.sample_rate_hz = visual->sample_rate_hz;
  request.quality = visual->quality;
  request.rules = visual->rules;
  const auto *signature_sets =
      reader.array(entry, "assetSignatures", path, Required::yes);
  if (signature_sets == nullptr) {
    error = reader_error(reader);
    return std::nullopt;
  }
  if (signature_sets->size() != visual->templates.size()) {
    error = "assetSignatures must match the visual template count";
    return std::nullopt;
  }
  request.templates.reserve(visual->templates.size());
  for (std::size_t template_index = 0;
       template_index < visual->templates.size(); ++template_index) {
    const auto signature_path = path + ".assetSignatures[" +
                                std::to_string(template_index) + "]";
    auto evaluation = decode_template_evaluation(
        (*signature_sets)[template_index], visual->templates[template_index],
        reader, signature_path, error);
    if (!evaluation)
      return std::nullopt;
    request.templates.push_back(std::move(*evaluation));
  }
  return request;
}

std::optional<RECT> decode_exclusion_rect(const Value &entry,
                                          const std::string &path,
                                          std::string &error) {
  if (!entry.is_array() || entry.as_array().size() != 4) {
    error = path + ": exclusion must contain four coordinates";
    return std::nullopt;
  }
  RECT rect{};
  long *coordinates[] = {&rect.left, &rect.top, &rect.right, &rect.bottom};
  for (std::size_t coordinate = 0; coordinate < 4; ++coordinate) {
    const auto &value = entry.as_array()[coordinate];
    if (!value.is_number() || !std::isfinite(value.as_number()) ||
        value.as_number() < std::numeric_limits<long>::min() ||
        value.as_number() > std::numeric_limits<long>::max()) {
      error = path + ": invalid exclusion coordinate";
      return std::nullopt;
    }
    *coordinates[coordinate] = static_cast<long>(value.as_number());
  }
  return rect;
}

IsolatedInboundMessage decode_configure_message(const Value &root,
                                                Reader &reader) {
  IsolatedConfiguration result;
  const auto sequence = wire_unsigned(reader, root, "sequence", "$");
  const auto quality = reader.integer(
      root, "captureQuality", "$", 0, layerc::core::capture_quality_max,
      Required::yes);
  const auto *requests = reader.array(root, "requests", "$", Required::yes);
  if (!sequence || !quality || requests == nullptr)
    return IsolatedProtocolError{reader_error(reader)};
  result.sequence = *sequence;
  result.capture_quality =
      static_cast<layerc::core::CaptureQuality>(*quality);
  if (requests->size() > 64)
    return IsolatedProtocolError{"requests must be an array of at most 64"};
  result.requests.reserve(requests->size());
  std::string error;
  for (std::size_t index = 0; index < requests->size(); ++index) {
    const auto path = "$.requests[" + std::to_string(index) + "]";
    auto request =
        decode_visual_request((*requests)[index], reader, path, error);
    if (!request)
      return IsolatedProtocolError{error};
    result.requests.push_back(std::move(*request));
  }
  const auto *exclusions = reader.array(root, "exclusions", "$", Required::yes);
  if (exclusions == nullptr)
    return IsolatedProtocolError{reader_error(reader)};
  if (exclusions->size() > 128)
    return IsolatedProtocolError{"exclusions must be an array of at most 128"};
  for (std::size_t index = 0; index < exclusions->size(); ++index) {
    const auto path = "$.exclusions[" + std::to_string(index) + "]";
    auto rect = decode_exclusion_rect((*exclusions)[index], path, error);
    if (!rect)
      return IsolatedProtocolError{error};
    if (rect->right > rect->left && rect->bottom > rect->top)
      result.exclusions.push_back(*rect);
  }
  return result;
}

IsolatedInboundMessage decode_poll_message(const Value &root, Reader &reader) {
  IsolatedPoll poll;
  const auto sequence = wire_unsigned(reader, root, "sequence", "$");
  if (!sequence)
    return IsolatedProtocolError{reader_error(reader)};
  poll.sequence = *sequence;
  return poll;
}

} // namespace

IsolatedSnapshotDisposition
IsolatedSnapshotValidator::validate(VisualCaptureSnapshot &snapshot,
                                    const layerc::core::TimePoint now,
                                    std::string &error) {
  if (snapshot.sequence < capture_sequence_) {
    error = "isolated capture sequence moved backwards";
    return IsolatedSnapshotDisposition::rejected;
  }
  constexpr auto maximum_age = std::chrono::seconds(2);
  constexpr auto maximum_future_skew = std::chrono::milliseconds(250);
  for (const auto &evaluation : snapshot.evaluations) {
    const auto previous = evaluation_sequences_.find(evaluation.source_key);
    if (previous != evaluation_sequences_.end() &&
        evaluation.sequence < previous->second) {
      error = "isolated evaluation sequence moved backwards";
      return IsolatedSnapshotDisposition::rejected;
    }
    for (const auto &measurement : evaluation.measurements) {
      if (measurement.sampled_at == layerc::core::TimePoint{} ||
          measurement.sampled_at > now + maximum_future_skew ||
          now - measurement.sampled_at > maximum_age) {
        error = "isolated measurement timestamp is stale or invalid";
        return IsolatedSnapshotDisposition::rejected;
      }
    }
  }
  if (snapshot.sequence == capture_sequence_)
    return IsolatedSnapshotDisposition::duplicate;

  std::erase_if(snapshot.evaluations, [&](const auto &evaluation) {
    const auto previous = evaluation_sequences_.find(evaluation.source_key);
    return previous != evaluation_sequences_.end() &&
           evaluation.sequence == previous->second;
  });
  capture_sequence_ = snapshot.sequence;
  for (const auto &evaluation : snapshot.evaluations)
    evaluation_sequences_[evaluation.source_key] = evaluation.sequence;
  return IsolatedSnapshotDisposition::accepted;
}

bool IsolatedSnapshotValidator::rebase(VisualCaptureSnapshot &snapshot) {
  const bool advanced =
      snapshot.sequence != 0 && snapshot.sequence != last_worker_sequence_;
  if (advanced) {
    last_worker_sequence_ = snapshot.sequence;
    snapshot.sequence = ++capture_sequence_;
    for (auto &evaluation : snapshot.evaluations)
      evaluation.sequence = ++evaluation_sequences_[evaluation.source_key];
    return true;
  }
  snapshot.sequence = capture_sequence_;
  for (auto &evaluation : snapshot.evaluations) {
    const auto found = evaluation_sequences_.find(evaluation.source_key);
    evaluation.sequence =
        found == evaluation_sequences_.end() ? 0U : found->second;
  }
  return false;
}

std::string isolated_hello_json(const std::string_view nonce,
                                const std::string_view role,
                                const std::uint32_t process_id) {
  std::string output;
  append_packet_prefix(output, "hello");
  output += ",\"nonce\":";
  layerc::core::json::append_quoted(output, nonce);
  output += ",\"role\":";
  layerc::core::json::append_quoted(output, role);
  output += ",\"processId\":";
  layerc::core::json::append_unsigned(output, process_id);
  output.push_back('}');
  return output;
}

bool validate_isolated_hello(const std::string_view json,
                             const std::string_view nonce,
                             const std::string_view expected_role,
                             const std::uint32_t expected_process_id,
                             std::string &error) {
  Reader reader;
  layerc::core::json::ParseResult parsed;
  const auto *root = object_root(json, reader, parsed);
  if (root == nullptr) {
    error = reader_error(reader);
    return false;
  }
  std::string type, actual_nonce, role;
  const auto hello_fields_ok =
      reader.string(*root, "type", "$", type, Required::yes,
                    EmptyString::reject) &&
      reader.string(*root, "nonce", "$", actual_nonce, Required::yes,
                    EmptyString::allow) &&
      reader.string(*root, "role", "$", role, Required::yes,
                    EmptyString::reject);
  const auto process_id =
      wire_unsigned(reader, *root, "processId", "$",
                    std::numeric_limits<std::uint32_t>::max());
  if (!hello_fields_ok || !process_id) {
    error = reader_error(reader);
    return false;
  }
  if (type != "hello") {
    error = "$.type: expected hello";
    return false;
  }
  if (actual_nonce != nonce || role != expected_role ||
      *process_id != expected_process_id) {
    error = "isolated visual hello identity mismatch";
    return false;
  }
  return true;
}

namespace {

void append_asset_signature(std::string &output,
                            const VisualAssetSignature &signature) {
  output += signature.present ? "{\"present\":true" : "{\"present\":false";
  output += ",\"volumeSerial\":";
  layerc::core::json::append_unsigned(output, signature.volume_serial);
  output += ",\"fileIndex\":";
  layerc::core::json::append_unsigned(output, signature.file_index);
  output += ",\"fileSize\":";
  layerc::core::json::append_unsigned(output, signature.file_size);
  output += ",\"lastWriteTicks\":";
  layerc::core::json::append_unsigned(output, signature.last_write_ticks);
  output += ",\"changeUsn\":";
  layerc::core::json::append_unsigned(output, signature.change_usn);
  output.push_back('}');
}

void append_exclusion_rect(std::string &output, const RECT &rect) {
  output.push_back('[');
  layerc::core::json::append_number(output, rect.left);
  output.push_back(',');
  layerc::core::json::append_number(output, rect.top);
  output.push_back(',');
  layerc::core::json::append_number(output, rect.right);
  output.push_back(',');
  layerc::core::json::append_number(output, rect.bottom);
  output.push_back(']');
}

} // namespace

std::string
encode_isolated_configuration(const IsolatedConfiguration &configuration) {
  std::string output;
  append_packet_prefix(output, "configure");
  output += ",\"sequence\":";
  layerc::core::json::append_unsigned(output, configuration.sequence);
  output += ",\"captureQuality\":";
  layerc::core::json::append_unsigned(
      output, static_cast<std::uint8_t>(configuration.capture_quality));
  output += ",\"requests\":[";
  for (std::size_t index = 0; index < configuration.requests.size(); ++index) {
    if (index != 0)
      output.push_back(',');
    const auto &request = configuration.requests[index];
    layerc::core::VisualProfile visual;
    visual.profile_id = "ipc";
    visual.sample_rate_hz = request.sample_rate_hz;
    visual.quality = request.quality;
    visual.rules = request.rules;
    visual.templates.reserve(request.templates.size());
    for (const auto &evaluation : request.templates)
      visual.templates.push_back(evaluation.state);
    output += "{\"sourceKey\":";
    layerc::core::json::append_quoted(output, request.source_key);
    output += ",\"visualProfile\":";
    append_visual_profile_object(output, visual);
    output += ",\"assetSignatures\":[";
    for (std::size_t template_index = 0;
         template_index < request.templates.size(); ++template_index) {
      if (template_index != 0)
        output.push_back(',');
      output.push_back('[');
      const auto &signatures =
          request.templates[template_index].asset_signatures;
      for (std::size_t signature_index = 0; signature_index < signatures.size();
           ++signature_index) {
        if (signature_index != 0)
          output.push_back(',');
        append_asset_signature(output, signatures[signature_index]);
      }
      output.push_back(']');
    }
    output.push_back(']');
    output.push_back('}');
  }
  output += "],\"exclusions\":[";
  for (std::size_t index = 0; index < configuration.exclusions.size();
       ++index) {
    if (index != 0)
      output.push_back(',');
    append_exclusion_rect(output, configuration.exclusions[index]);
  }
  output += "]}";
  return output;
}

IsolatedInboundMessage decode_isolated_message(const std::string_view json) {
  Reader reader;
  layerc::core::json::ParseResult parsed;
  const auto *root = object_root(json, reader, parsed);
  if (root == nullptr)
    return IsolatedProtocolError{reader_error(reader)};
  const auto type = reader.string(*root, "type", "$", Required::yes,
                                 EmptyString::reject);
  if (!type)
    return IsolatedProtocolError{reader_error(reader)};
  if (*type == "configure")
    return decode_configure_message(*root, reader);
  if (*type == "waitSnapshot")
    return decode_poll_message(*root, reader);
  return IsolatedProtocolError{"unexpected isolated visual message type"};
}

std::string encode_isolated_poll(const std::uint64_t sequence) {
  return encode_sequence_packet("waitSnapshot", sequence);
}

std::string encode_isolated_snapshot(const std::uint64_t packet_sequence,
                                     const VisualCaptureSnapshot &snapshot) {
  std::string output;
  append_packet_prefix(output, "snapshot");
  output += ",\"sequence\":";
  layerc::core::json::append_unsigned(output, packet_sequence);
  output += ",\"captureSequence\":";
  layerc::core::json::append_unsigned(output, snapshot.sequence);
  output += ",\"availability\":";
  layerc::core::json::append_unsigned(
      output, static_cast<std::uint8_t>(snapshot.result.availability));
  output += ",\"backend\":";
  layerc::core::json::append_unsigned(
      output, static_cast<std::uint8_t>(snapshot.result.backend));
  output += ",\"gpu\":";
  layerc::core::json::append_quoted(output, snapshot.gpu_description);
  output += ",\"evaluations\":[";
  for (std::size_t index = 0; index < snapshot.evaluations.size(); ++index) {
    if (index != 0)
      output.push_back(',');
    const auto &evaluation = snapshot.evaluations[index];
    output += "{\"sourceKey\":";
    layerc::core::json::append_quoted(output, evaluation.source_key);
    output += ",\"sequence\":";
    layerc::core::json::append_unsigned(output, evaluation.sequence);
    output += evaluation.missing_template_assets ? ",\"missingAssets\":true"
                                                 : ",\"missingAssets\":false";
    output += ",\"measurements\":[";
    for (std::size_t measurement_index = 0;
         measurement_index < evaluation.measurements.size();
         ++measurement_index) {
      if (measurement_index != 0)
        output.push_back(',');
      const auto &measurement = evaluation.measurements[measurement_index];
      output += "{\"id\":";
      layerc::core::json::append_quoted(output, measurement.observation_id);
      output += ",\"availability\":";
      layerc::core::json::append_unsigned(
          output, static_cast<std::uint8_t>(measurement.availability));
      output +=
          measurement.matched ? ",\"matched\":true" : ",\"matched\":false";
      output += ",\"confidence\":";
      layerc::core::json::append_number(output, measurement.confidence);
      output += ",\"sampledAtNs\":";
      layerc::core::json::append_unsigned(
          output, monotonic_nanoseconds(measurement.sampled_at));
      output.push_back('}');
    }
    output += "]}";
  }
  output += "]}";
  return output;
}

namespace {

std::optional<layerc::core::ObservationMeasurement>
decode_measurement(const Value &measurement_value, Reader &reader,
                   const std::string &measurement_path, std::string &error) {
  if (!measurement_value.is_object()) {
    error = measurement_path + ": measurement must be an object";
    return std::nullopt;
  }
  layerc::core::ObservationMeasurement measurement;
  bool matched{};
  if (!reader.string(measurement_value, "id", measurement_path,
                     measurement.observation_id, Required::yes,
                     EmptyString::reject) ||
      !reader.boolean(measurement_value, "matched", measurement_path, matched,
                      Required::yes)) {
    error = reader_error(reader);
    return std::nullopt;
  }
  const auto sampled_at =
      wire_unsigned(reader, measurement_value, "sampledAtNs", measurement_path);
  const auto confidence = reader.number(measurement_value, "confidence",
                                        measurement_path, 0.0, 1.0,
                                        Required::yes);
  const auto measurement_availability = reader.integer(
      measurement_value, "availability", measurement_path, 0,
      layerc::core::capture_availability_max, Required::yes);
  if (!sampled_at || !confidence || !measurement_availability) {
    error = reader_error(reader);
    return std::nullopt;
  }
  measurement.availability =
      static_cast<layerc::core::CaptureAvailability>(*measurement_availability);
  measurement.matched = matched;
  measurement.confidence = *confidence;
  measurement.sampled_at =
      layerc::core::TimePoint(std::chrono::nanoseconds(*sampled_at));
  return measurement;
}

std::optional<VisualEvaluationSnapshot>
decode_evaluation(const Value &entry, Reader &reader, const std::string &path,
                  std::string &error) {
  if (!entry.is_object()) {
    error = path + ": isolated evaluation must be an object";
    return std::nullopt;
  }
  std::string source_key;
  bool missing{};
  if (!reader.string(entry, "sourceKey", path, source_key, Required::yes,
                     EmptyString::reject) ||
      !reader.boolean(entry, "missingAssets", path, missing, Required::yes)) {
    error = reader_error(reader);
    return std::nullopt;
  }
  const auto sequence = wire_unsigned(reader, entry, "sequence", path);
  const auto *measurements =
      reader.array(entry, "measurements", path, Required::yes);
  if (!sequence || measurements == nullptr) {
    error = reader_error(reader);
    return std::nullopt;
  }
  if (measurements->size() > 8) {
    error = "invalid isolated measurements";
    return std::nullopt;
  }
  VisualEvaluationSnapshot evaluation;
  evaluation.source_key = std::move(source_key);
  evaluation.sequence = *sequence;
  evaluation.missing_template_assets = missing;
  for (std::size_t measurement_index = 0;
       measurement_index < measurements->size(); ++measurement_index) {
    const auto measurement_path =
        path + ".measurements[" + std::to_string(measurement_index) + "]";
    auto measurement = decode_measurement((*measurements)[measurement_index],
                                          reader, measurement_path, error);
    if (!measurement)
      return std::nullopt;
    evaluation.measurements.push_back(std::move(*measurement));
  }
  return evaluation;
}

} // namespace

IsolatedSnapshotResult
decode_isolated_snapshot(const std::string_view json,
                         const std::uint64_t expected_packet_sequence) {
  Reader reader;
  layerc::core::json::ParseResult parsed;
  const auto *root = object_root(json, reader, parsed);
  if (root == nullptr)
    return IsolatedProtocolError{reader_error(reader)};
  std::string type, gpu;
  if (!reader.string(*root, "type", "$", type, Required::yes,
                     EmptyString::reject) ||
      !reader.string(*root, "gpu", "$", gpu, Required::yes,
                     EmptyString::allow)) {
    return IsolatedProtocolError{reader_error(reader)};
  }
  const auto packet_sequence = wire_unsigned(reader, *root, "sequence", "$");
  const auto capture_sequence =
      wire_unsigned(reader, *root, "captureSequence", "$");
  const auto availability = reader.integer(
      *root, "availability", "$", 0, layerc::core::capture_availability_max,
      Required::yes);
  const auto backend = reader.integer(*root, "backend", "$", 0,
                                      isolated_capture_backend_max,
                                      Required::yes);
  const auto *evaluations = reader.array(*root, "evaluations", "$",
                                         Required::yes);
  if (!reader.ok() || !packet_sequence || !capture_sequence || !availability ||
      !backend || evaluations == nullptr) {
    if (reader.ok())
      return IsolatedProtocolError{"invalid isolated capture status"};
    return IsolatedProtocolError{reader_error(reader)};
  }
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
  result.gpu_description = std::move(gpu);
  std::string error;
  for (std::size_t index = 0; index < evaluations->size(); ++index) {
    const auto path = "$.evaluations[" + std::to_string(index) + "]";
    auto evaluation =
        decode_evaluation((*evaluations)[index], reader, path, error);
    if (!evaluation)
      return IsolatedProtocolError{error};
    result.evaluations.push_back(std::move(*evaluation));
  }
  return result;
}

std::string encode_isolated_ack(const std::uint64_t sequence) {
  return encode_sequence_packet("ack", sequence);
}

bool decode_isolated_ack(const std::string_view json,
                         const std::uint64_t expected_sequence,
                         std::string &error) {
  Reader reader;
  layerc::core::json::ParseResult parsed;
  const auto *root = object_root(json, reader, parsed);
  if (root == nullptr) {
    error = reader_error(reader);
    return false;
  }
  std::string type;
  const auto type_ok = reader.string(*root, "type", "$", type, Required::yes,
                                     EmptyString::reject);
  const auto sequence = wire_unsigned(reader, *root, "sequence", "$");
  if (!type_ok || !sequence) {
    error = reader_error(reader);
    return false;
  }
  if (type != "ack" || *sequence != expected_sequence) {
    error = "invalid isolated acknowledgement";
    return false;
  }
  return true;
}

} // namespace layerc::daemon
