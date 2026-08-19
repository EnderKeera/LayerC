#include "isolated_visual_protocol.hpp"

#include "layerc/core/json.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/text.hpp"

#include <charconv>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace layerc::daemon {
namespace {

using layerc::core::json::Value;

const Value *object_root(const std::string_view source, std::string &error,
                         layerc::core::json::ParseResult &storage) {
  if (source.size() > isolated_visual_max_message_bytes) {
    error = "isolated visual message exceeds 1 MiB";
    return nullptr;
  }
  storage = layerc::core::json::parse(source);
  if (const auto *parse_error =
          std::get_if<layerc::core::json::ParseError>(&storage)) {
    error = "invalid isolated visual JSON at byte " +
            std::to_string(parse_error->offset) + ": " +
            parse_error->message;
    return nullptr;
  }
  const auto &root = std::get<Value>(storage);
  if (!root.is_object()) {
    error = "isolated visual message root must be an object";
    return nullptr;
  }
  const auto *version = root.find("protocolVersion");
  if (version == nullptr || !version->is_number() ||
      version->as_number() != isolated_visual_protocol_version) {
    error = "isolated visual protocol version mismatch";
    return nullptr;
  }
  const auto *package = root.find("packageVersion");
  if (package == nullptr || !package->is_string() ||
      package->as_string() != layerc::core::package_version) {
    error = "isolated visual package version mismatch";
    return nullptr;
  }
  return &root;
}

bool string_value(const Value &root, const std::string_view name,
                  std::string &output, std::string &error) {
  const auto *value = root.find(name);
  if (value == nullptr || !value->is_string()) {
    error = std::string(name) + " must be a string";
    return false;
  }
  output = value->as_string();
  return true;
}

bool decimal_value(const Value &root, const std::string_view name,
                   std::uint64_t &output, std::string &error) {
  std::string source;
  if (!string_value(root, name, source, error)) return false;
  const auto [end, code] = std::from_chars(
      source.data(), source.data() + source.size(), output);
  if (code != std::errc{} || end != source.data() + source.size()) {
    error = std::string(name) + " must be an unsigned decimal string";
    return false;
  }
  return true;
}

bool bool_value(const Value &root, const std::string_view name, bool &output,
                std::string &error) {
  const auto *value = root.find(name);
  if (value == nullptr || !value->is_bool()) {
    error = std::string(name) + " must be a boolean";
    return false;
  }
  output = value->as_bool();
  return true;
}

bool number_value(const Value &root, const std::string_view name,
                  double &output, std::string &error) {
  const auto *value = root.find(name);
  if (value == nullptr || !value->is_number() ||
      !std::isfinite(value->as_number())) {
    error = std::string(name) + " must be a finite number";
    return false;
  }
  output = value->as_number();
  return true;
}

void append_packet_prefix(std::string &output, const std::string_view type) {
  output += "{\"protocolVersion\":";
  layerc::core::json::append_unsigned(output,
                                      isolated_visual_protocol_version);
  output += ",\"packageVersion\":";
  layerc::core::json::append_quoted(output, layerc::core::package_version);
  output += ",\"type\":";
  layerc::core::json::append_quoted(output, type);
}

void append_decimal(std::string &output, const std::uint64_t value) {
  output.push_back('"');
  layerc::core::json::append_unsigned(output, value);
  output.push_back('"');
}

std::uint64_t monotonic_nanoseconds(const layerc::core::TimePoint value) {
  const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         value.time_since_epoch())
                         .count();
  return count < 0 ? 0U : static_cast<std::uint64_t>(count);
}

} // namespace

IsolatedSnapshotDisposition IsolatedSnapshotValidator::validate(
    VisualCaptureSnapshot &snapshot, const layerc::core::TimePoint now,
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

bool IsolatedSnapshotSequencer::rebase(VisualCaptureSnapshot &snapshot) {
  const bool advanced = snapshot.sequence != 0 &&
                        snapshot.sequence != last_worker_sequence_;
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
  append_decimal(output, process_id);
  output.push_back('}');
  return output;
}

bool validate_isolated_hello(const std::string_view json,
                             const std::string_view nonce,
                             const std::string_view expected_role,
                             const std::uint32_t expected_process_id,
                             std::string &error) {
  layerc::core::json::ParseResult parsed;
  const auto *root = object_root(json, error, parsed);
  if (root == nullptr) return false;
  std::string type, actual_nonce, role;
  std::uint64_t process_id{};
  if (!string_value(*root, "type", type, error) || type != "hello" ||
      !string_value(*root, "nonce", actual_nonce, error) ||
      !string_value(*root, "role", role, error) ||
      !decimal_value(*root, "processId", process_id, error)) {
    if (error.empty()) error = "invalid isolated visual hello";
    return false;
  }
  if (actual_nonce != nonce || role != expected_role ||
      process_id != expected_process_id) {
    error = "isolated visual hello identity mismatch";
    return false;
  }
  return true;
}

std::string encode_isolated_configuration(
    const IsolatedConfiguration &configuration) {
  std::string output;
  append_packet_prefix(output, "configure");
  output += ",\"sequence\":";
  append_decimal(output, configuration.sequence);
  output += ",\"captureQuality\":";
  layerc::core::json::append_unsigned(
      output, static_cast<std::uint8_t>(configuration.capture_quality));
  output += ",\"requests\":[";
  for (std::size_t index = 0; index < configuration.requests.size(); ++index) {
    if (index != 0) output.push_back(',');
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
    layerc::core::json::append_quoted(
        output, layerc::platform::wide_to_utf8(request.source_key));
    output += ",\"visualJson\":";
    layerc::core::json::append_quoted(
        output, layerc::core::serialize_visual_profile_json(visual));
    output += ",\"assetSignatures\":[";
    for (std::size_t template_index = 0;
         template_index < request.templates.size(); ++template_index) {
      if (template_index != 0) output.push_back(',');
      output.push_back('[');
      const auto &signatures =
          request.templates[template_index].asset_signatures;
      for (std::size_t signature_index = 0;
           signature_index < signatures.size(); ++signature_index) {
        if (signature_index != 0) output.push_back(',');
        const auto &signature = signatures[signature_index];
        output += signature.present ? "{\"present\":true"
                                    : "{\"present\":false";
        output += ",\"volumeSerial\":";
        append_decimal(output, signature.volume_serial);
        output += ",\"fileIndex\":";
        append_decimal(output, signature.file_index);
        output += ",\"fileSize\":";
        append_decimal(output, signature.file_size);
        output += ",\"lastWriteTicks\":";
        append_decimal(output, signature.last_write_ticks);
        output += ",\"changeUsn\":";
        append_decimal(output, signature.change_usn);
        output.push_back('}');
      }
      output.push_back(']');
    }
    output.push_back(']');
    output.push_back('}');
  }
  output += "],\"exclusions\":[";
  for (std::size_t index = 0; index < configuration.exclusions.size();
       ++index) {
    if (index != 0) output.push_back(',');
    const auto &rect = configuration.exclusions[index];
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
  output += "]}";
  return output;
}

IsolatedConfigurationResult
decode_isolated_configuration(const std::string_view json) {
  std::string error;
  layerc::core::json::ParseResult parsed;
  const auto *root = object_root(json, error, parsed);
  if (root == nullptr) return IsolatedProtocolError{std::move(error)};
  std::string type;
  IsolatedConfiguration result;
  if (!string_value(*root, "type", type, error) || type != "configure" ||
      !decimal_value(*root, "sequence", result.sequence, error)) {
    if (error.empty()) error = "expected isolated configure message";
    return IsolatedProtocolError{std::move(error)};
  }
  const auto *quality = root->find("captureQuality");
  if (quality == nullptr || !quality->is_number() ||
      quality->as_number() < 0 || quality->as_number() > 2) {
    return IsolatedProtocolError{"captureQuality must be 0, 1, or 2"};
  }
  result.capture_quality =
      static_cast<layerc::core::CaptureQuality>(quality->as_number());
  const auto *requests = root->find("requests");
  if (requests == nullptr || !requests->is_array() ||
      requests->as_array().size() > 64) {
    return IsolatedProtocolError{"requests must be an array of at most 64"};
  }
  result.requests.reserve(requests->as_array().size());
  for (const auto &entry : requests->as_array()) {
    if (!entry.is_object())
      return IsolatedProtocolError{"request must be an object"};
    std::string source_key, visual_json;
    if (!string_value(entry, "sourceKey", source_key, error) ||
        !string_value(entry, "visualJson", visual_json, error))
      return IsolatedProtocolError{std::move(error)};
    layerc::core::GameProfile base;
    base.id = "ipc";
    const auto decoded =
        layerc::core::parse_visual_profile_json(visual_json, base);
    const auto *visual = std::get_if<layerc::core::VisualProfile>(&decoded);
    if (visual == nullptr)
      return IsolatedProtocolError{"invalid visual evaluation request"};
    VisualEvaluationRequest request{
        .source_key = layerc::platform::utf8_to_wide(source_key),
        .sample_rate_hz = visual->sample_rate_hz,
        .quality = visual->quality,
        .rules = visual->rules};
    const auto *signature_sets = entry.find("assetSignatures");
    if (signature_sets == nullptr || !signature_sets->is_array() ||
        signature_sets->as_array().size() != visual->templates.size())
      return IsolatedProtocolError{
          "assetSignatures must match the visual template count"};
    request.templates.reserve(visual->templates.size());
    for (std::size_t template_index = 0;
         template_index < visual->templates.size(); ++template_index) {
      const auto &state = visual->templates[template_index];
      const auto &encoded_signatures =
          signature_sets->as_array()[template_index];
      if (!encoded_signatures.is_array() ||
          encoded_signatures.as_array().size() != state.references.size())
        return IsolatedProtocolError{
            "asset signatures must match template references"};
      VisualTemplateEvaluation evaluation{.state = state};
      evaluation.asset_signatures.reserve(
          encoded_signatures.as_array().size());
      for (const auto &encoded : encoded_signatures.as_array()) {
        if (!encoded.is_object())
          return IsolatedProtocolError{"asset signature must be an object"};
        VisualAssetSignature signature;
        std::uint64_t volume_serial{}, file_index{}, file_size{},
            write_ticks{}, change_usn{};
        if (!bool_value(encoded, "present", signature.present, error) ||
            !decimal_value(encoded, "volumeSerial", volume_serial, error) ||
            !decimal_value(encoded, "fileIndex", file_index, error) ||
            !decimal_value(encoded, "fileSize", file_size, error) ||
            !decimal_value(encoded, "lastWriteTicks", write_ticks, error) ||
            !decimal_value(encoded, "changeUsn", change_usn, error) ||
            volume_serial > std::numeric_limits<std::uint32_t>::max())
          return IsolatedProtocolError{
              error.empty() ? "invalid asset signature" : std::move(error)};
        signature.volume_serial = static_cast<std::uint32_t>(volume_serial);
        signature.file_index = file_index;
        signature.file_size = file_size;
        signature.last_write_ticks = write_ticks;
        signature.change_usn = change_usn;
        evaluation.asset_signatures.push_back(signature);
      }
      request.templates.push_back(std::move(evaluation));
    }
    result.requests.push_back(std::move(request));
  }
  const auto *exclusions = root->find("exclusions");
  if (exclusions == nullptr || !exclusions->is_array() ||
      exclusions->as_array().size() > 128)
    return IsolatedProtocolError{"exclusions must be an array of at most 128"};
  for (const auto &entry : exclusions->as_array()) {
    if (!entry.is_array() || entry.as_array().size() != 4)
      return IsolatedProtocolError{"exclusion must contain four coordinates"};
    RECT rect{};
    long *coordinates[] = {&rect.left, &rect.top, &rect.right, &rect.bottom};
    for (std::size_t index = 0; index < 4; ++index) {
      const auto &value = entry.as_array()[index];
      if (!value.is_number() || !std::isfinite(value.as_number()) ||
          value.as_number() < std::numeric_limits<long>::min() ||
          value.as_number() > std::numeric_limits<long>::max())
        return IsolatedProtocolError{"invalid exclusion coordinate"};
      *coordinates[index] = static_cast<long>(value.as_number());
    }
    if (rect.right > rect.left && rect.bottom > rect.top)
      result.exclusions.push_back(rect);
  }
  return result;
}

std::string encode_isolated_poll(const std::uint64_t sequence) {
  std::string output;
  append_packet_prefix(output, "waitSnapshot");
  output += ",\"sequence\":";
  append_decimal(output, sequence);
  output.push_back('}');
  return output;
}

bool decode_isolated_poll(const std::string_view json, std::uint64_t &sequence,
                          std::string &error) {
  layerc::core::json::ParseResult parsed;
  const auto *root = object_root(json, error, parsed);
  if (root == nullptr) return false;
  std::string type;
  if (!string_value(*root, "type", type, error) ||
      type != "waitSnapshot" ||
      !decimal_value(*root, "sequence", sequence, error)) {
    if (error.empty()) error = "expected isolated waitSnapshot message";
    return false;
  }
  return true;
}

std::string encode_isolated_snapshot(
    const std::uint64_t packet_sequence,
    const VisualCaptureSnapshot &snapshot) {
  std::string output;
  append_packet_prefix(output, "snapshot");
  output += ",\"sequence\":";
  append_decimal(output, packet_sequence);
  output += ",\"captureSequence\":";
  append_decimal(output, snapshot.sequence);
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
    if (index != 0) output.push_back(',');
    const auto &evaluation = snapshot.evaluations[index];
    output += "{\"sourceKey\":";
    layerc::core::json::append_quoted(
        output, layerc::platform::wide_to_utf8(evaluation.source_key));
    output += ",\"sequence\":";
    append_decimal(output, evaluation.sequence);
    output += evaluation.missing_template_assets
                  ? ",\"missingAssets\":true"
                  : ",\"missingAssets\":false";
    output += ",\"measurements\":[";
    for (std::size_t measurement_index = 0;
         measurement_index < evaluation.measurements.size();
         ++measurement_index) {
      if (measurement_index != 0) output.push_back(',');
      const auto &measurement = evaluation.measurements[measurement_index];
      output += "{\"id\":";
      layerc::core::json::append_quoted(output, measurement.observation_id);
      output += ",\"availability\":";
      layerc::core::json::append_unsigned(
          output, static_cast<std::uint8_t>(measurement.availability));
      output += measurement.matched ? ",\"matched\":true"
                                    : ",\"matched\":false";
      output += ",\"confidence\":";
      layerc::core::json::append_number(output, measurement.confidence);
      output += ",\"sampledAtNs\":";
      append_decimal(output, monotonic_nanoseconds(measurement.sampled_at));
      output.push_back('}');
    }
    output += "]}";
  }
  output += "]}";
  return output;
}

IsolatedSnapshotResult
decode_isolated_snapshot(const std::string_view json,
                         const std::uint64_t expected_packet_sequence) {
  std::string error;
  layerc::core::json::ParseResult parsed;
  const auto *root = object_root(json, error, parsed);
  if (root == nullptr) return IsolatedProtocolError{std::move(error)};
  std::string type, gpu;
  std::uint64_t packet_sequence{}, capture_sequence{};
  if (!string_value(*root, "type", type, error) || type != "snapshot" ||
      !decimal_value(*root, "sequence", packet_sequence, error) ||
      !decimal_value(*root, "captureSequence", capture_sequence, error) ||
      !string_value(*root, "gpu", gpu, error)) {
    if (error.empty()) error = "invalid isolated snapshot";
    return IsolatedProtocolError{std::move(error)};
  }
  if (packet_sequence != expected_packet_sequence)
    return IsolatedProtocolError{"out-of-order isolated snapshot"};
  const auto *availability = root->find("availability");
  const auto *backend = root->find("backend");
  if (availability == nullptr || !availability->is_number() ||
      availability->as_number() < 0 || availability->as_number() > 5 ||
      backend == nullptr || !backend->is_number() ||
      backend->as_number() < 0 || backend->as_number() > 2)
    return IsolatedProtocolError{"invalid isolated capture status"};
  VisualCaptureSnapshot result;
  result.sequence = capture_sequence;
  result.result.availability = static_cast<layerc::core::CaptureAvailability>(
      availability->as_number());
  result.result.backend = static_cast<layerc::core::CaptureBackendKind>(
      backend->as_number());
  result.gpu_description = std::move(gpu);
  const auto *evaluations = root->find("evaluations");
  if (evaluations == nullptr || !evaluations->is_array() ||
      evaluations->as_array().size() > 64)
    return IsolatedProtocolError{"invalid isolated evaluations"};
  for (const auto &entry : evaluations->as_array()) {
    if (!entry.is_object())
      return IsolatedProtocolError{"isolated evaluation must be an object"};
    std::string source_key;
    std::uint64_t sequence{};
    bool missing{};
    if (!string_value(entry, "sourceKey", source_key, error) ||
        !decimal_value(entry, "sequence", sequence, error) ||
        !bool_value(entry, "missingAssets", missing, error))
      return IsolatedProtocolError{std::move(error)};
    VisualEvaluationSnapshot evaluation{
        .source_key = layerc::platform::utf8_to_wide(source_key),
        .sequence = sequence,
        .missing_template_assets = missing};
    const auto *measurements = entry.find("measurements");
    if (measurements == nullptr || !measurements->is_array() ||
        measurements->as_array().size() > 8)
      return IsolatedProtocolError{"invalid isolated measurements"};
    for (const auto &measurement_value : measurements->as_array()) {
      if (!measurement_value.is_object())
        return IsolatedProtocolError{"measurement must be an object"};
      layerc::core::ObservationMeasurement measurement;
      std::uint64_t sampled_at{};
      double confidence{};
      bool matched{};
      if (!string_value(measurement_value, "id", measurement.observation_id,
                        error) ||
          !bool_value(measurement_value, "matched", matched, error) ||
          !number_value(measurement_value, "confidence", confidence, error) ||
          !decimal_value(measurement_value, "sampledAtNs", sampled_at,
                         error))
        return IsolatedProtocolError{std::move(error)};
      const auto *measurement_availability =
          measurement_value.find("availability");
      if (measurement_availability == nullptr ||
          !measurement_availability->is_number() ||
          measurement_availability->as_number() < 0 ||
          measurement_availability->as_number() > 5 || confidence < 0.0 ||
          confidence > 1.0)
        return IsolatedProtocolError{"invalid isolated measurement values"};
      measurement.availability =
          static_cast<layerc::core::CaptureAvailability>(
              measurement_availability->as_number());
      measurement.matched = matched;
      measurement.confidence = confidence;
      measurement.sampled_at = layerc::core::TimePoint(
          std::chrono::nanoseconds(sampled_at));
      evaluation.measurements.push_back(std::move(measurement));
    }
    result.evaluations.push_back(std::move(evaluation));
  }
  return result;
}

std::string encode_isolated_ack(const std::uint64_t sequence) {
  std::string output;
  append_packet_prefix(output, "ack");
  output += ",\"sequence\":";
  append_decimal(output, sequence);
  output.push_back('}');
  return output;
}

bool decode_isolated_ack(const std::string_view json,
                         const std::uint64_t expected_sequence,
                         std::string &error) {
  layerc::core::json::ParseResult parsed;
  const auto *root = object_root(json, error, parsed);
  if (root == nullptr) return false;
  std::string type;
  std::uint64_t sequence{};
  if (!string_value(*root, "type", type, error) || type != "ack" ||
      !decimal_value(*root, "sequence", sequence, error) ||
      sequence != expected_sequence) {
    if (error.empty()) error = "invalid isolated acknowledgement";
    return false;
  }
  return true;
}

} // namespace layerc::daemon
