#include "layerc/core/live_edit.hpp"

#include "layerc/core/frame.hpp"
#include "layerc/core/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <optional>

namespace layerc::core {
namespace {

bool same_runtime_definition(const CooldownDefinition &left,
                             const CooldownDefinition &right) {
  return left.id == right.id && left.duration == right.duration &&
         left.max_charges == right.max_charges &&
         left.retrigger == right.retrigger && left.recovery == right.recovery;
}

const CooldownDefinition *
find_cooldown(const std::vector<CooldownDefinition> &definitions,
              const std::string_view id) {
  const auto found = std::ranges::find_if(
      definitions,
      [&](const CooldownDefinition &candidate) { return candidate.id == id; });
  return found == definitions.end() ? nullptr : &*found;
}

std::string base64_encode(const std::string_view source) {
  constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve(((source.size() + 2U) / 3U) * 4U);
  for (std::size_t offset = 0; offset < source.size(); offset += 3U) {
    const auto first = static_cast<unsigned char>(source[offset]);
    const auto second = offset + 1U < source.size()
                            ? static_cast<unsigned char>(source[offset + 1U])
                            : 0U;
    const auto third = offset + 2U < source.size()
                           ? static_cast<unsigned char>(source[offset + 2U])
                           : 0U;
    const std::uint32_t value = (static_cast<std::uint32_t>(first) << 16U) |
                                (static_cast<std::uint32_t>(second) << 8U) |
                                static_cast<std::uint32_t>(third);
    output.push_back(alphabet[(value >> 18U) & 0x3FU]);
    output.push_back(alphabet[(value >> 12U) & 0x3FU]);
    output.push_back(
        offset + 1U < source.size() ? alphabet[(value >> 6U) & 0x3FU] : '=');
    output.push_back(offset + 2U < source.size() ? alphabet[value & 0x3FU]
                                                 : '=');
  }
  return output;
}

std::optional<std::string> base64_decode(const std::string_view source) {
  if (source.size() % 4U != 0)
    return std::nullopt;
  const auto value_of = [](const char character) -> int {
    if (character >= 'A' && character <= 'Z')
      return character - 'A';
    if (character >= 'a' && character <= 'z')
      return character - 'a' + 26;
    if (character >= '0' && character <= '9')
      return character - '0' + 52;
    if (character == '+')
      return 62;
    if (character == '/')
      return 63;
    return -1;
  };
  std::string output;
  output.reserve(source.size() / 4U * 3U);
  for (std::size_t offset = 0; offset < source.size(); offset += 4U) {
    const bool last = offset + 4U == source.size();
    const bool pad_two = source[offset + 2U] == '=';
    const bool pad_three = source[offset + 3U] == '=';
    if ((!last && (pad_two || pad_three)) || (pad_two && !pad_three))
      return std::nullopt;
    const int first = value_of(source[offset]);
    const int second = value_of(source[offset + 1U]);
    const int third = pad_two ? 0 : value_of(source[offset + 2U]);
    const int fourth = pad_three ? 0 : value_of(source[offset + 3U]);
    if (first < 0 || second < 0 || third < 0 || fourth < 0)
      return std::nullopt;
    const std::uint32_t value = (static_cast<std::uint32_t>(first) << 18U) |
                                (static_cast<std::uint32_t>(second) << 12U) |
                                (static_cast<std::uint32_t>(third) << 6U) |
                                static_cast<std::uint32_t>(fourth);
    output.push_back(static_cast<char>((value >> 16U) & 0xFFU));
    if (!pad_two)
      output.push_back(static_cast<char>((value >> 8U) & 0xFFU));
    if (!pad_three)
      output.push_back(static_cast<char>(value & 0xFFU));
  }
  return output;
}

std::string_view message_type_name(const LiveMessageType type) {
  switch (type) {
  case LiveMessageType::handshake:
    return "handshake";
  case LiveMessageType::committed_profile:
    return "committedProfile";
  case LiveMessageType::begin_edit:
    return "beginEdit";
  case LiveMessageType::live_profile:
    return "liveProfile";
  case LiveMessageType::select_panel:
    return "selectPanel";
  case LiveMessageType::anchor_changed:
    return "anchorChanged";
  case LiveMessageType::set_direct_drag:
    return "setDirectDrag";
  case LiveMessageType::end_edit:
    return "endEdit";
  case LiveMessageType::heartbeat:
    return "heartbeat";
  case LiveMessageType::runtime_status:
    return "runtimeStatus";
  case LiveMessageType::diagnostics_request:
    return "diagnosticsRequest";
  case LiveMessageType::diagnostics_reply:
    return "diagnosticsReply";
  case LiveMessageType::retry_visual:
    return "retryVisual";
  case LiveMessageType::automation_trace_subscribe:
    return "automationTraceSubscribe";
  case LiveMessageType::automation_trace_unsubscribe:
    return "automationTraceUnsubscribe";
  case LiveMessageType::automation_trace_event:
    return "automationTraceEvent";
  case LiveMessageType::retry_automation:
    return "retryAutomation";
  case LiveMessageType::acknowledgement:
    return "ack";
  case LiveMessageType::error:
    return "error";
  }
  return "error";
}

std::optional<LiveMessageType> parse_message_type(const std::string_view name) {
  static constexpr std::pair<std::string_view, LiveMessageType> names[] = {
      {"handshake", LiveMessageType::handshake},
      {"committedProfile", LiveMessageType::committed_profile},
      {"beginEdit", LiveMessageType::begin_edit},
      {"liveProfile", LiveMessageType::live_profile},
      {"selectPanel", LiveMessageType::select_panel},
      {"anchorChanged", LiveMessageType::anchor_changed},
      {"setDirectDrag", LiveMessageType::set_direct_drag},
      {"endEdit", LiveMessageType::end_edit},
      {"heartbeat", LiveMessageType::heartbeat},
      {"runtimeStatus", LiveMessageType::runtime_status},
      {"diagnosticsRequest", LiveMessageType::diagnostics_request},
      {"diagnosticsReply", LiveMessageType::diagnostics_reply},
      {"retryVisual", LiveMessageType::retry_visual},
      {"automationTraceSubscribe", LiveMessageType::automation_trace_subscribe},
      {"automationTraceUnsubscribe",
       LiveMessageType::automation_trace_unsubscribe},
      {"automationTraceEvent", LiveMessageType::automation_trace_event},
      {"retryAutomation", LiveMessageType::retry_automation},
      {"ack", LiveMessageType::acknowledgement},
      {"error", LiveMessageType::error},
  };
  const auto found = std::ranges::find_if(
      names, [&](const auto &entry) { return entry.first == name; });
  return found == std::end(names) ? std::nullopt : std::optional(found->second);
}

bool string_field(const json::Value &root, const std::string_view name,
                  std::string &output, std::string &error) {
  const auto *value = root.find(name);
  if (value == nullptr)
    return true;
  if (!value->is_string()) {
    error = std::string(name) + " must be a string";
    return false;
  }
  output = value->as_string();
  return true;
}

bool bool_field(const json::Value &root, const std::string_view name,
                bool &output, std::string &error) {
  const auto *value = root.find(name);
  if (value == nullptr)
    return true;
  if (!value->is_bool()) {
    error = std::string(name) + " must be a boolean";
    return false;
  }
  output = value->as_bool();
  return true;
}

bool double_field(const json::Value &root, const std::string_view name,
                  double &output, std::string &error) {
  const auto *value = root.find(name);
  if (value == nullptr)
    return true;
  if (!value->is_number() || !std::isfinite(value->as_number())) {
    error = std::string(name) + " must be a finite number";
    return false;
  }
  output = value->as_number();
  return true;
}

template <typename Integer>
bool decimal_string_field(const json::Value &root, const std::string_view name,
                          Integer &output, std::string &error) {
  const auto *value = root.find(name);
  if (value == nullptr)
    return true;
  if (!value->is_string()) {
    error = std::string(name) + " must be a decimal string";
    return false;
  }
  const auto &source = value->as_string();
  Integer parsed{};
  const auto [end, code] =
      std::from_chars(source.data(), source.data() + source.size(), parsed);
  if (code != std::errc{} || end != source.data() + source.size()) {
    error = std::string(name) + " must be an unsigned decimal string";
    return false;
  }
  output = parsed;
  return true;
}

bool require_fields(const json::Value &root,
                    const std::initializer_list<std::string_view> names,
                    std::string &error) {
  for (const auto name : names) {
    if (root.find(name) != nullptr)
      continue;
    error = std::string(name) + " is required";
    return false;
  }
  return true;
}

} // namespace

RevisionDecision revision_decision(const ProfileRevision incoming,
                                   const ProfileRevision current) {
  if (incoming > current)
    return RevisionDecision::accept;
  return incoming == current ? RevisionDecision::duplicate
                             : RevisionDecision::stale;
}

bool ProfileDelta::empty() const noexcept {
  return !metadata_changed && !enablement_changed && !matcher_changed &&
         !bindings_changed && !panels_changed && !cooldown_visuals_changed &&
         added_cooldown_ids.empty() && removed_cooldown_ids.empty() &&
         reset_cooldown_ids.empty();
}

ProfileDelta diff_profiles(const GameProfile &before,
                           const GameProfile &after) {
  ProfileDelta delta;
  delta.metadata_changed = before.schema_version != after.schema_version ||
                           before.id != after.id || before.name != after.name ||
                           before.visual_runtime != after.visual_runtime ||
                           before.modules != after.modules;
  delta.enablement_changed = before.enabled != after.enabled;
  delta.matcher_changed = before.game != after.game;
  delta.bindings_changed = before.bindings != after.bindings ||
                           before.hid_bindings != after.hid_bindings;
  delta.panels_changed = before.panels != after.panels;

  for (const auto &definition : before.cooldowns) {
    const auto *updated = find_cooldown(after.cooldowns, definition.id);
    if (updated == nullptr) {
      delta.removed_cooldown_ids.push_back(definition.id);
    } else {
      if (!same_runtime_definition(definition, *updated))
        delta.reset_cooldown_ids.push_back(definition.id);
      if (definition.name != updated->name)
        delta.cooldown_visuals_changed = true;
    }
  }
  for (const auto &definition : after.cooldowns) {
    if (find_cooldown(before.cooldowns, definition.id) == nullptr)
      delta.added_cooldown_ids.push_back(definition.id);
  }
  return delta;
}

SessionReconcilePlan plan_session_reconciliation(const GameProfile &before,
                                                 const GameProfile &after) {
  const auto delta = diff_profiles(before, after);
  SessionReconcilePlan plan;
  plan.recreate_instances = delta.enablement_changed || delta.matcher_changed ||
                            before.visual_runtime != after.visual_runtime ||
                            before.modules != after.modules;
  plan.rebuild_input_matcher = delta.bindings_changed ||
                               !delta.added_cooldown_ids.empty() ||
                               !delta.removed_cooldown_ids.empty();
  plan.rebuild_overlays =
      delta.panels_changed || delta.cooldown_visuals_changed ||
      !delta.added_cooldown_ids.empty() ||
      !delta.removed_cooldown_ids.empty() || !delta.reset_cooldown_ids.empty();
  plan.added_cooldown_ids = delta.added_cooldown_ids;
  plan.removed_cooldown_ids = delta.removed_cooldown_ids;
  plan.reset_cooldown_ids = delta.reset_cooldown_ids;
  for (const auto &definition : after.cooldowns) {
    if (std::ranges::find(plan.added_cooldown_ids, definition.id) ==
            plan.added_cooldown_ids.end() &&
        std::ranges::find(plan.reset_cooldown_ids, definition.id) ==
            plan.reset_cooldown_ids.end())
      plan.preserved_cooldown_ids.push_back(definition.id);
  }
  return plan;
}

NormalizedAnchor normalized_anchor_for_drag(const long pointer_x,
                                            const long pointer_y,
                                            const long grab_offset_x,
                                            const long grab_offset_y,
                                            const PixelRect &target) {
  const auto width = std::max(1L, target.right - target.left);
  const auto height = std::max(1L, target.bottom - target.top);
  return {
      .x = std::clamp((pointer_x - grab_offset_x - target.left) /
                          static_cast<double>(width),
                      0.0, 1.0),
      .y = std::clamp((pointer_y - grab_offset_y - target.top) /
                          static_cast<double>(height),
                      0.0, 1.0),
  };
}

PixelRect overlay_rectangle(const NormalizedAnchor anchor,
                            const PixelRect &target, const long width,
                            const long height) {
  const auto target_width = std::max(1L, target.right - target.left);
  const auto target_height = std::max(1L, target.bottom - target.top);
  const auto center_x =
      target.left + static_cast<long>(std::lround(
                        std::clamp(anchor.x, 0.0, 1.0) * target_width));
  const auto center_y =
      target.top + static_cast<long>(std::lround(
                       std::clamp(anchor.y, 0.0, 1.0) * target_height));
  const auto safe_width = std::max(1L, width);
  const auto safe_height = std::max(1L, height);
  return {.left = center_x - safe_width / 2,
          .top = center_y - safe_height / 2,
          .right = center_x - safe_width / 2 + safe_width,
          .bottom = center_y - safe_height / 2 + safe_height};
}

std::string encode_live_message_json(const LiveMessage &message) {
  const auto encoded_profile = base64_encode(message.profile_json);
  std::string output;
  output.reserve(256U + message.profile_key.size() + message.panel_id.size() +
                 encoded_profile.size() + message.text.size());
  const auto append_quoted_unsigned = [&](const std::uint64_t value) {
    output.push_back('"');
    json::append_unsigned(output, value);
    output.push_back('"');
  };

  output += "{\"protocolVersion\":";
  json::append_unsigned(output, message.protocol_version);
  output += ",\"type\":";
  json::append_quoted(output, message_type_name(message.type));
  output += ",\"revision\":";
  append_quoted_unsigned(message.revision);
  output += ",\"editSession\":";
  append_quoted_unsigned(message.edit_session);
  output += ",\"targetWindow\":";
  append_quoted_unsigned(message.target_window);
  output += ",\"profileKey\":";
  json::append_quoted(output, message.profile_key);
  output += ",\"panelId\":";
  json::append_quoted(output, message.panel_id);
  output += ",\"profileBase64\":";
  json::append_quoted(output, encoded_profile);
  output += ",\"text\":";
  json::append_quoted(output, message.text);
  output += ",\"anchorX\":";
  json::append_number(output, message.anchor_x);
  output += ",\"anchorY\":";
  json::append_number(output, message.anchor_y);
  output +=
      message.direct_drag ? ",\"directDrag\":true" : ",\"directDrag\":false";
  output += message.final ? ",\"final\":true}" : ",\"final\":false}";
  return output;
}

LiveMessageResult decode_live_message_json(const std::string_view source) {
  if (source.size() > max_live_message_bytes)
    return LiveProtocolError{"live message exceeds the 512 KiB limit"};
  const auto parsed = json::parse(source);
  if (const auto *parse_error = std::get_if<json::ParseError>(&parsed)) {
    return LiveProtocolError{"invalid live message JSON at byte " +
                             std::to_string(parse_error->offset) + ": " +
                             parse_error->message};
  }
  const auto &root = std::get<json::Value>(parsed);
  if (!root.is_object())
    return LiveProtocolError{"live message root must be an object"};
  const auto *version = root.find("protocolVersion");
  const auto *type = root.find("type");
  if (version == nullptr || !version->is_number() ||
      version->as_number() != live_protocol_version)
    return LiveProtocolError{"unsupported live protocol version"};
  if (type == nullptr || !type->is_string())
    return LiveProtocolError{"live message type is required"};
  const auto parsed_type = parse_message_type(type->as_string());
  if (!parsed_type.has_value())
    return LiveProtocolError{"unknown live message type"};

  LiveMessage message;
  message.type = *parsed_type;
  std::string error;
  if (!require_fields(root, {"revision"}, error))
    return LiveProtocolError{std::move(error)};
  switch (message.type) {
  case LiveMessageType::handshake:
    if (!require_fields(root, {"text"}, error))
      return LiveProtocolError{std::move(error)};
    break;
  case LiveMessageType::committed_profile:
    if (!require_fields(root, {"profileKey", "profileBase64"}, error))
      return LiveProtocolError{std::move(error)};
    break;
  case LiveMessageType::begin_edit:
    if (!require_fields(root,
                        {"editSession", "targetWindow", "profileKey", "panelId",
                         "profileBase64"},
                        error))
      return LiveProtocolError{std::move(error)};
    break;
  case LiveMessageType::live_profile:
    if (!require_fields(root, {"editSession", "profileBase64"}, error))
      return LiveProtocolError{std::move(error)};
    break;
  case LiveMessageType::select_panel:
    if (!require_fields(root, {"editSession", "panelId"}, error))
      return LiveProtocolError{std::move(error)};
    break;
  case LiveMessageType::anchor_changed:
    if (!require_fields(
            root, {"editSession", "panelId", "anchorX", "anchorY", "final"},
            error))
      return LiveProtocolError{std::move(error)};
    break;
  case LiveMessageType::set_direct_drag:
    if (!require_fields(root, {"editSession", "directDrag"}, error))
      return LiveProtocolError{std::move(error)};
    break;
  case LiveMessageType::end_edit:
  case LiveMessageType::heartbeat:
    if (!require_fields(root, {"editSession"}, error))
      return LiveProtocolError{std::move(error)};
    break;
  case LiveMessageType::runtime_status:
  case LiveMessageType::diagnostics_reply:
    if (!require_fields(root, {"text"}, error))
      return LiveProtocolError{std::move(error)};
    break;
  case LiveMessageType::diagnostics_request:
  case LiveMessageType::retry_visual:
  case LiveMessageType::automation_trace_subscribe:
  case LiveMessageType::automation_trace_unsubscribe:
    break;
  case LiveMessageType::automation_trace_event:
    if (!require_fields(root, {"profileKey", "text"}, error))
      return LiveProtocolError{std::move(error)};
    break;
  case LiveMessageType::retry_automation:
    if (!require_fields(root, {"profileKey", "text"}, error))
      return LiveProtocolError{std::move(error)};
    break;
  case LiveMessageType::acknowledgement:
  case LiveMessageType::error:
    if (!require_fields(root, {"text"}, error))
      return LiveProtocolError{std::move(error)};
    break;
  }
  std::string encoded_profile;
  if (!decimal_string_field(root, "revision", message.revision, error) ||
      !decimal_string_field(root, "editSession", message.edit_session, error) ||
      !decimal_string_field(root, "targetWindow", message.target_window,
                            error) ||
      !string_field(root, "profileKey", message.profile_key, error) ||
      !string_field(root, "panelId", message.panel_id, error) ||
      !string_field(root, "profileBase64", encoded_profile, error) ||
      !string_field(root, "text", message.text, error) ||
      !double_field(root, "anchorX", message.anchor_x, error) ||
      !double_field(root, "anchorY", message.anchor_y, error) ||
      !bool_field(root, "directDrag", message.direct_drag, error) ||
      !bool_field(root, "final", message.final, error))
    return LiveProtocolError{std::move(error)};
  const auto decoded_profile = base64_decode(encoded_profile);
  if (!decoded_profile.has_value())
    return LiveProtocolError{"profileBase64 is invalid"};
  message.profile_json = *decoded_profile;
  if (message.anchor_x < 0.0 || message.anchor_x > 1.0 ||
      message.anchor_y < 0.0 || message.anchor_y > 1.0)
    return LiveProtocolError{"anchor coordinates must be normalized"};
  return message;
}

LiveFrameResult encode_live_frame(const LiveMessage &message) {
  const auto payload = encode_live_message_json(message);
  if (payload.size() > max_live_message_bytes)
    return LiveProtocolError{"live message exceeds the 512 KiB limit"};
  const auto length = static_cast<std::uint32_t>(payload.size());
  std::vector<std::byte> frame(frame_header_bytes + payload.size());
  append_frame_header(frame.data(), length);
  std::memcpy(frame.data() + frame_header_bytes, payload.data(),
              payload.size());
  return frame;
}

std::vector<LiveMessageResult>
LiveFrameDecoder::push(const std::span<const std::byte> bytes) {
  if (bytes.size() > max_live_buffer_bytes ||
      buffer_.size() > max_live_buffer_bytes - bytes.size()) {
    clear();
    return {LiveProtocolError{"live frame buffer exceeds the 4 MiB limit"}};
  }
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
  std::vector<LiveMessageResult> results;
  std::size_t consumed{};
  while (buffer_.size() - consumed >= frame_header_bytes) {
    const std::uint32_t length =
        read_frame_header(buffer_.data() + consumed);
    if (length > max_live_message_bytes) {
      results.emplace_back(
          LiveProtocolError{"live frame exceeds the 512 KiB limit"});
      clear();
      return results;
    }
    if (buffer_.size() - consumed < frame_header_bytes + length)
      break;
    const auto *payload = reinterpret_cast<const char *>(
        buffer_.data() + consumed + frame_header_bytes);
    results.push_back(
        decode_live_message_json(std::string_view(payload, length)));
    consumed += frame_header_bytes + length;
  }
  if (consumed != 0) {
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
  }
  return results;
}

} // namespace layerc::core
