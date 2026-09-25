#include "layerc/core/live_edit.hpp"

#include "layerc/core/base64.hpp"
#include "layerc/core/enum_wire.hpp"
#include "layerc/core/frame.hpp"
#include "layerc/core/json_reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
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
  const auto found =
      std::ranges::find(definitions, id, &CooldownDefinition::id);
  return found == definitions.end() ? nullptr : &*found;
}

static constexpr std::array k_message_names{
    EnumWireEntry{LiveMessageType::handshake, "handshake"},
    EnumWireEntry{LiveMessageType::committed_profile, "committedProfile"},
    EnumWireEntry{LiveMessageType::begin_edit, "beginEdit"},
    EnumWireEntry{LiveMessageType::live_profile, "liveProfile"},
    EnumWireEntry{LiveMessageType::select_panel, "selectPanel"},
    EnumWireEntry{LiveMessageType::anchor_changed, "anchorChanged"},
    EnumWireEntry{LiveMessageType::set_direct_drag, "setDirectDrag"},
    EnumWireEntry{LiveMessageType::end_edit, "endEdit"},
    EnumWireEntry{LiveMessageType::heartbeat, "heartbeat"},
    EnumWireEntry{LiveMessageType::runtime_status, "runtimeStatus"},
    EnumWireEntry{LiveMessageType::diagnostics_request, "diagnosticsRequest"},
    EnumWireEntry{LiveMessageType::diagnostics_reply, "diagnosticsReply"},
    EnumWireEntry{LiveMessageType::retry_visual, "retryVisual"},
    EnumWireEntry{LiveMessageType::automation_trace_subscribe,
                  "automationTraceSubscribe"},
    EnumWireEntry{LiveMessageType::automation_trace_unsubscribe,
                  "automationTraceUnsubscribe"},
    EnumWireEntry{LiveMessageType::automation_trace_event,
                  "automationTraceEvent"},
    EnumWireEntry{LiveMessageType::retry_automation, "retryAutomation"},
    EnumWireEntry{LiveMessageType::acknowledgement, "ack"},
    EnumWireEntry{LiveMessageType::error, "error"},
};

std::string_view message_type_name(const LiveMessageType type) {
  return enum_wire_name<LiveMessageType>(k_message_names, type)
      .value_or("error");
}

std::optional<LiveMessageType> parse_message_type(const std::string_view name) {
  return parse_enum_wire<LiveMessageType>(k_message_names, name);
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
  json::Reader fields;
  if (!fields.required_fields(root, {"revision"}, "$"))
    return LiveProtocolError{fields.error().describe()};
  switch (message.type) {
  case LiveMessageType::handshake:
  case LiveMessageType::runtime_status:
  case LiveMessageType::diagnostics_reply:
  case LiveMessageType::acknowledgement:
  case LiveMessageType::error:
    if (!fields.required_fields(root, {"text"}, "$"))
      return LiveProtocolError{fields.error().describe()};
    break;
  case LiveMessageType::committed_profile:
    if (!fields.required_fields(root, {"profileKey", "profileBase64"}, "$"))
      return LiveProtocolError{fields.error().describe()};
    break;
  case LiveMessageType::begin_edit:
    if (!fields.required_fields(root,
                        {"editSession", "targetWindow", "profileKey", "panelId",
                         "profileBase64"},
                        "$"))
      return LiveProtocolError{fields.error().describe()};
    break;
  case LiveMessageType::live_profile:
    if (!fields.required_fields(root, {"editSession", "profileBase64"}, "$"))
      return LiveProtocolError{fields.error().describe()};
    break;
  case LiveMessageType::select_panel:
    if (!fields.required_fields(root, {"editSession", "panelId"}, "$"))
      return LiveProtocolError{fields.error().describe()};
    break;
  case LiveMessageType::anchor_changed:
    if (!fields.required_fields(
            root, {"editSession", "panelId", "anchorX", "anchorY", "final"},
            "$"))
      return LiveProtocolError{fields.error().describe()};
    break;
  case LiveMessageType::set_direct_drag:
    if (!fields.required_fields(root, {"editSession", "directDrag"}, "$"))
      return LiveProtocolError{fields.error().describe()};
    break;
  case LiveMessageType::end_edit:
  case LiveMessageType::heartbeat:
    if (!fields.required_fields(root, {"editSession"}, "$"))
      return LiveProtocolError{fields.error().describe()};
    break;
  case LiveMessageType::diagnostics_request:
  case LiveMessageType::retry_visual:
  case LiveMessageType::automation_trace_subscribe:
  case LiveMessageType::automation_trace_unsubscribe:
    break;
  case LiveMessageType::automation_trace_event:
  case LiveMessageType::retry_automation:
    if (!fields.required_fields(root, {"profileKey", "text"}, "$"))
      return LiveProtocolError{fields.error().describe()};
    break;
  }
  std::string encoded_profile;
  const auto revision = fields.unsigned_integer(
      root, "revision", "$", std::numeric_limits<std::uint64_t>::max(),
      json::Required::yes, 0, json::IntegerEncoding::decimal_string);
  const auto session = fields.unsigned_integer(
      root, "editSession", "$", std::numeric_limits<std::uint64_t>::max(),
      json::Required::no, message.edit_session, json::IntegerEncoding::decimal_string);
  const auto window = fields.unsigned_integer(
      root, "targetWindow", "$", std::numeric_limits<std::uint64_t>::max(),
      json::Required::no, message.target_window, json::IntegerEncoding::decimal_string);
  const auto anchor_x = fields.number(root, "anchorX", "$", 0.0, 1.0,
                                      json::Required::no, message.anchor_x);
  const auto anchor_y = fields.number(root, "anchorY", "$", 0.0, 1.0,
                                      json::Required::no, message.anchor_y);
  if (!revision || !session || !window || !anchor_x || !anchor_y ||
      !fields.string(root, "profileKey", "$", message.profile_key,
                     json::Required::no, json::EmptyString::allow) ||
      !fields.string(root, "panelId", "$", message.panel_id,
                     json::Required::no, json::EmptyString::allow) ||
      !fields.string(root, "profileBase64", "$", encoded_profile,
                     json::Required::no, json::EmptyString::allow) ||
      !fields.string(root, "text", "$", message.text,
                     json::Required::no, json::EmptyString::allow) ||
      !fields.boolean(root, "directDrag", "$", message.direct_drag, json::Required::no) ||
      !fields.boolean(root, "final", "$", message.final, json::Required::no))
    return LiveProtocolError{fields.error().describe()};
  message.revision = *revision;
  message.edit_session = *session;
  message.target_window = *window;
  message.anchor_x = *anchor_x;
  message.anchor_y = *anchor_y;
  const auto decoded_profile = base64_decode(encoded_profile);
  if (!decoded_profile.has_value())
    return LiveProtocolError{"profileBase64 is invalid"};
  message.profile_json = *decoded_profile;
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
  decoder_.feed(bytes);
  std::vector<LiveMessageResult> results;
  while (const auto payload = decoder_.next_frame())
    results.push_back(decode_live_message_json(*payload));
  if (decoder_.fault() != FrameDecoder::Fault::none) {
    results.emplace_back(LiveProtocolError{
        decoder_.fault() == FrameDecoder::Fault::buffer_too_large
            ? "live frame buffer exceeds the 4 MiB limit"
            : "live frame exceeds the 512 KiB limit"});
    clear();
  }
  return results;
}

} // namespace layerc::core
