#include "layerc/core/live_edit.hpp"

#include "layerc/core/automation.hpp"

#include "layerc/core/base64.hpp"
#include "layerc/core/cooldown_groups.hpp"
#include "layerc/core/frame.hpp"
#include "layerc/core/json_reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

namespace layerc::core {
namespace {

const CooldownDefinition *
find_cooldown(const std::vector<CooldownDefinition> &definitions,
              const std::string_view id) {
  const auto found =
      std::ranges::find(definitions, id, &CooldownDefinition::id);
  return found == definitions.end() ? nullptr : &*found;
}

// The live protocol's one message table: each type's wire name and the
// fields it carries. The encoder writes exactly these fields and the decoder
// requires exactly these, beside protocolVersion, type, and revision.
enum LiveField : std::uint16_t {
  edit_session_field = 1U << 0U,
  target_window_field = 1U << 1U,
  profile_key_field = 1U << 2U,
  panel_id_field = 1U << 3U,
  profile_field = 1U << 4U,
  text_field = 1U << 5U,
  anchor_field = 1U << 6U,
  final_field = 1U << 7U,
  direct_drag_field = 1U << 8U,
};

struct LiveFieldKeys {
  LiveField field;
  std::array<std::string_view, 2> keys;
};

constexpr std::array k_field_keys{
    LiveFieldKeys{edit_session_field, {"editSession"}},
    LiveFieldKeys{target_window_field, {"targetWindow"}},
    LiveFieldKeys{profile_key_field, {"profileKey"}},
    LiveFieldKeys{panel_id_field, {"panelId"}},
    LiveFieldKeys{profile_field, {"profileBase64"}},
    LiveFieldKeys{text_field, {"text"}},
    LiveFieldKeys{anchor_field, {"anchorX", "anchorY"}},
    LiveFieldKeys{final_field, {"final"}},
    LiveFieldKeys{direct_drag_field, {"directDrag"}},
};

struct LiveMessageSpec {
  LiveMessageType type;
  std::string_view wire;
  std::uint16_t fields;
};

// Replies (acknowledgement, error, endEdit) echo the request's edit session
// and profile key so the editor can attribute them.
constexpr std::uint16_t reply_fields =
    edit_session_field | profile_key_field | text_field;

constexpr std::array k_message_specs{
    LiveMessageSpec{LiveMessageType::handshake, "handshake", text_field},
    LiveMessageSpec{LiveMessageType::committed_profile, "committedProfile",
                    profile_key_field | profile_field},
    LiveMessageSpec{LiveMessageType::begin_edit, "beginEdit",
                    edit_session_field | target_window_field |
                        profile_key_field | panel_id_field | profile_field},
    LiveMessageSpec{LiveMessageType::live_profile, "liveProfile",
                    edit_session_field | profile_field},
    LiveMessageSpec{LiveMessageType::select_panel, "selectPanel",
                    edit_session_field | panel_id_field},
    LiveMessageSpec{LiveMessageType::anchor_changed, "anchorChanged",
                    edit_session_field | panel_id_field | anchor_field |
                        final_field},
    LiveMessageSpec{LiveMessageType::set_direct_drag, "setDirectDrag",
                    edit_session_field | direct_drag_field},
    LiveMessageSpec{LiveMessageType::end_edit, "endEdit", reply_fields},
    LiveMessageSpec{LiveMessageType::heartbeat, "heartbeat",
                    edit_session_field},
    LiveMessageSpec{LiveMessageType::runtime_status, "runtimeStatus",
                    text_field},
    LiveMessageSpec{LiveMessageType::diagnostics_request,
                    "diagnosticsRequest", 0},
    LiveMessageSpec{LiveMessageType::diagnostics_reply, "diagnosticsReply",
                    text_field},
    LiveMessageSpec{LiveMessageType::retry_visual, "retryVisual", 0},
    LiveMessageSpec{LiveMessageType::automation_trace_subscribe,
                    "automationTraceSubscribe", 0},
    LiveMessageSpec{LiveMessageType::automation_trace_unsubscribe,
                    "automationTraceUnsubscribe", 0},
    LiveMessageSpec{LiveMessageType::automation_trace_event,
                    "automationTraceEvent", profile_key_field | text_field},
    LiveMessageSpec{LiveMessageType::retry_automation, "retryAutomation",
                    profile_key_field | text_field},
    LiveMessageSpec{LiveMessageType::acknowledgement, "ack", reply_fields},
    LiveMessageSpec{LiveMessageType::error, "error", reply_fields},
};

[[nodiscard]] const LiveMessageSpec &message_spec(const LiveMessageType type) {
  const auto found = std::ranges::find(k_message_specs, type,
                                       &LiveMessageSpec::type);
  return found == k_message_specs.end() ? k_message_specs.back() : *found;
}

[[nodiscard]] const LiveMessageSpec *
find_message_spec(const std::string_view wire) {
  const auto found = std::ranges::find(k_message_specs, wire,
                                       &LiveMessageSpec::wire);
  return found == k_message_specs.end() ? nullptr : &*found;
}

[[nodiscard]] bool field_key(const std::uint16_t fields,
                             const std::string_view key) {
  return !key.empty() &&
         std::ranges::any_of(k_field_keys, [&](const LiveFieldKeys &entry) {
    return (fields & entry.field) != 0U &&
           std::ranges::find(entry.keys, key) != entry.keys.end();
  });
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
  delta.instance_shape_changed = before.visual_runtime != after.visual_runtime ||
                                 before.modules != after.modules;
  delta.metadata_changed = delta.instance_shape_changed ||
                           before.schema_version != after.schema_version ||
                           before.id != after.id || before.name != after.name ||
                           before.cooldown_groups != after.cooldown_groups ||
                           before.cooldowns != after.cooldowns;
  delta.enablement_changed = before.enabled != after.enabled;
  delta.matcher_changed = before.game != after.game;
  delta.bindings_changed = before.bindings != after.bindings ||
                           before.hid_bindings != after.hid_bindings;
  delta.panels_changed = before.panels != after.panels;

  // Pure owner resolution: an unvalidated profile cannot throw here.
  const CooldownGroupTopology previous(before.cooldown_groups);
  const CooldownGroupTopology replacement(after.cooldown_groups);
  for (const auto &definition : before.cooldowns) {
    const auto *updated = find_cooldown(after.cooldowns, definition.id);
    if (updated == nullptr) {
      delta.removed_cooldown_ids.push_back(definition.id);
    } else {
      if (previous.owner(before.cooldowns, definition.id) !=
          replacement.owner(after.cooldowns, definition.id))
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
  auto delta = diff_profiles(before, after);
  SessionReconcilePlan plan;
  plan.recreate_instances = delta.enablement_changed || delta.matcher_changed ||
                            delta.instance_shape_changed;
  plan.rebuild_input_matcher = delta.bindings_changed ||
                               !delta.added_cooldown_ids.empty() ||
                               !delta.removed_cooldown_ids.empty();
  plan.rebuild_overlays =
      delta.panels_changed || delta.cooldown_visuals_changed ||
      !delta.added_cooldown_ids.empty() ||
      !delta.removed_cooldown_ids.empty() || !delta.reset_cooldown_ids.empty();
  plan.added_cooldown_ids = std::move(delta.added_cooldown_ids);
  plan.removed_cooldown_ids = std::move(delta.removed_cooldown_ids);
  plan.reset_cooldown_ids = std::move(delta.reset_cooldown_ids);
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
  const auto &spec = message_spec(message.type);
  const auto carries = [&](const LiveField field) {
    return (spec.fields & field) != 0U;
  };
  const auto encoded_profile = carries(profile_field)
                                   ? base64_encode(message.profile_json)
                                   : std::string{};
  std::string output;
  output.reserve(192U + message.profile_key.size() + message.panel_id.size() +
                 encoded_profile.size() + message.text.size());
  const auto append_quoted_unsigned = [&](const std::uint64_t value) {
    output.push_back('"');
    json::append_unsigned(output, value);
    output.push_back('"');
  };

  output += "{\"protocolVersion\":";
  json::append_unsigned(output, message.protocol_version);
  output += ",\"type\":";
  json::append_quoted(output, spec.wire);
  output += ",\"revision\":";
  append_quoted_unsigned(message.revision);
  if (carries(edit_session_field)) {
    output += ",\"editSession\":";
    append_quoted_unsigned(message.edit_session);
  }
  if (carries(target_window_field)) {
    output += ",\"targetWindow\":";
    append_quoted_unsigned(message.target_window);
  }
  if (carries(profile_key_field)) {
    output += ",\"profileKey\":";
    json::append_quoted(output, message.profile_key);
  }
  if (carries(panel_id_field)) {
    output += ",\"panelId\":";
    json::append_quoted(output, message.panel_id);
  }
  if (carries(profile_field)) {
    output += ",\"profileBase64\":";
    json::append_quoted(output, encoded_profile);
  }
  if (carries(text_field)) {
    output += ",\"text\":";
    json::append_quoted(output, message.text);
  }
  if (carries(anchor_field)) {
    output += ",\"anchorX\":";
    json::append_number(output, message.anchor_x);
    output += ",\"anchorY\":";
    json::append_number(output, message.anchor_y);
  }
  if (carries(direct_drag_field))
    output += message.direct_drag ? ",\"directDrag\":true"
                                  : ",\"directDrag\":false";
  if (carries(final_field))
    output += message.final ? ",\"final\":true" : ",\"final\":false";
  output.push_back('}');
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
  const auto *spec = find_message_spec(type->as_string());
  if (spec == nullptr)
    return LiveProtocolError{"unknown live message type"};

  LiveMessage message;
  message.type = spec->type;
  json::Reader fields;
  for (const auto &[key, ignored] : root.as_object()) {
    (void)ignored;
    if (key != "protocolVersion" && key != "type" && key != "revision" &&
        !field_key(spec->fields, key))
      return LiveProtocolError{"$." + key + ": unknown field"};
  }
  // Every field the type carries is required; the others stay default.
  const auto required = [&](const LiveField field) {
    return (spec->fields & field) != 0U ? json::Required::yes
                                        : json::Required::no;
  };
  std::string encoded_profile;
  const auto revision = fields.unsigned_integer(
      root, "revision", "$", std::numeric_limits<std::uint64_t>::max(),
      json::Required::yes, 0, json::IntegerEncoding::decimal_string);
  const auto session = fields.unsigned_integer(
      root, "editSession", "$", std::numeric_limits<std::uint64_t>::max(),
      required(edit_session_field), message.edit_session,
      json::IntegerEncoding::decimal_string);
  const auto window = fields.unsigned_integer(
      root, "targetWindow", "$", std::numeric_limits<std::uint64_t>::max(),
      required(target_window_field), message.target_window,
      json::IntegerEncoding::decimal_string);
  const auto anchor_x = fields.number(root, "anchorX", "$", 0.0, 1.0,
                                      required(anchor_field), message.anchor_x);
  const auto anchor_y = fields.number(root, "anchorY", "$", 0.0, 1.0,
                                      required(anchor_field), message.anchor_y);
  if (!revision || !session || !window || !anchor_x || !anchor_y ||
      !fields.string(root, "profileKey", "$", message.profile_key,
                     required(profile_key_field), json::EmptyString::allow) ||
      !fields.string(root, "panelId", "$", message.panel_id,
                     required(panel_id_field), json::EmptyString::allow) ||
      !fields.string(root, "profileBase64", "$", encoded_profile,
                     required(profile_field), json::EmptyString::allow) ||
      !fields.string(root, "text", "$", message.text, required(text_field),
                     json::EmptyString::allow) ||
      !fields.boolean(root, "directDrag", "$", message.direct_drag,
                      required(direct_drag_field)) ||
      !fields.boolean(root, "final", "$", message.final, required(final_field)))
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

std::size_t
live_trace_prefix_count(const LiveMessage &base_message,
                        const std::span<const AutomationTraceEntry> entries) {
  // JSON string escaping is per byte, so the quoted "text" field grows by the
  // escaped size of each appended entry. Serialize each entry once and pack
  // greedily instead of re-encoding whole prefixes.
  const auto escaped_size = [](const std::string_view text) {
    std::string quoted;
    json::append_quoted(quoted, text);
    return quoted.size() - 2U;
  };
  auto message = base_message;
  message.text = export_layerc_trace({});
  auto encoded_size = encode_live_message_json(message).size();
  std::string entry_json;
  std::size_t selected{};
  for (const auto &entry : entries) {
    entry_json.clear();
    if (selected != 0U)
      entry_json.push_back(',');
    append_layerc_trace_entry(entry_json, entry);
    encoded_size += escaped_size(entry_json);
    if (encoded_size > max_live_message_bytes)
      break;
    ++selected;
  }
  return selected;
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
