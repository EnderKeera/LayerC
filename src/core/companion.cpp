#include "layerc/core/companion.hpp"
#include "layerc/core/json_reader.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>

namespace layerc::core {

namespace {
struct BoundedField {
  std::string_view name;
  std::string_view value;
  bool allow_empty;
};

std::optional<CompanionError> bounded_field_error(const std::string &path,
                                                  const BoundedField &field) {
  if ((!field.allow_empty && field.value.empty()) ||
      field.value.size() > max_companion_text_bytes)
    return CompanionError{path + "." + std::string(field.name),
                          "string is empty or exceeds its bound"};
  return std::nullopt;
}

// Companion text and integer-token policies are wire requirements; the
// generic type/range checks and path diagnostics live in json::Reader.
std::optional<CompanionError> required_string(
    const json::Value &object, const std::string_view key, const std::string &path,
    const bool allow_empty, std::string &out) {
  json::Reader fields;
  if (!fields.string(object, key, path, out, json::Required::yes,
                     allow_empty ? json::EmptyString::allow : json::EmptyString::reject,
                     max_companion_text_bytes))
    return CompanionError{fields.error().path, fields.error().message};
  return std::nullopt;
}
std::optional<CompanionError> required_integer(
    const json::Value &object, const std::string_view key, const std::string &path,
    const std::int64_t minimum, const std::int64_t maximum, std::int64_t &out) {
  json::Reader fields;
  const auto value = fields.integer(object, key, path, minimum, maximum,
                                    json::Required::yes, 0,
                                    json::IntegerEncoding::integer_token);
  if (!value) return CompanionError{fields.error().path, fields.error().message};
  out = *value;
  return std::nullopt;
}
std::optional<CompanionError> required_bool(
    const json::Value &object, const std::string_view key, const std::string &path,
    bool &out) {
  json::Reader fields;
  if (!fields.boolean(object, key, path, out, json::Required::yes))
    return CompanionError{fields.error().path, fields.error().message};
  return std::nullopt;
}

CompanionValidationResult
validate_slot_mapping(const CompanionSlotMapping &slot,
                      const GameProfile &base_profile,
                      const std::string &path) {
  if (slot.slot_index >= max_companion_slots)
    return CompanionError{path + ".slotIndex",
                          "slotIndex must be between 0 and 7"};
  if (slot.panel_id.empty() || slot.panel_id.size() > max_companion_text_bytes)
    return CompanionError{path + ".panelId",
                          "panelId must be a bounded non-empty string"};
  const bool panel_exists =
      std::ranges::any_of(base_profile.panels, [&](const Panel &p) {
        return p.id == slot.panel_id;
      });
  if (!panel_exists)
    return CompanionError{path + ".panelId",
                          "referenced panelId does not exist in base profile"};
  if (!std::isfinite(slot.opacity) || slot.opacity < 0.0 || slot.opacity > 1.0)
    return CompanionError{path + ".opacity",
                          "opacity must be finite and within [0,1]"};
  return std::nullopt;
}

CompanionValidationResult
parse_slot_mapping(const json::Value &value, const std::string &path,
                   const GameProfile &base_profile,
                   CompanionSlotMapping &slot) {
  json::Reader fields;
  const auto index = fields.integer(value, "slotIndex", path, 0,
                                    max_companion_slots - 1, json::Required::yes,
                                    0, json::IntegerEncoding::integer_token);
  const auto opacity = fields.number(value, "opacity", path, 0.0, 1.0,
                                     json::Required::no, slot.opacity);
  if (!index || !opacity ||
      !fields.string(value, "panelId", path, slot.panel_id, json::Required::yes,
                     json::EmptyString::reject, max_companion_text_bytes) ||
      !fields.boolean(value, "pinned", path, slot.pinned, json::Required::no) ||
      !fields.boolean(value, "clickThrough", path, slot.click_through, json::Required::no)) {
    return CompanionError{fields.error().path, fields.error().message};
  }
  slot.slot_index = static_cast<std::uint32_t>(*index);
  slot.opacity = *opacity;
  return validate_slot_mapping(slot, base_profile, path);
}

} // namespace

std::variant<CompanionProfile, CompanionError>
parse_companion_profile_json(const std::string_view json_str,
                             const GameProfile &base_profile) {
  if (json_str.size() > max_companion_sidecar_bytes) {
    return CompanionError{"$", "companion sidecar exceeds bounded size limit"};
  }
  auto parsed_result = json::parse(json_str);
  const auto *parsed = std::get_if<json::Value>(&parsed_result);
  if (parsed == nullptr || !parsed->is_object()) {
    return CompanionError{"$",
                          "invalid JSON root object for companion profile"};
  }
  return parse_companion_profile_value(*parsed, base_profile);
}

std::variant<CompanionProfile, CompanionError>
parse_companion_profile_value(const json::Value &value,
                              const GameProfile &base_profile) {
  if (!value.is_object())
    return CompanionError{"$", "companion profile root must be an object"};
  const auto &root = value.as_object();
  const auto schema_it = root.find("schemaVersion");
  if (schema_it == root.end() || !schema_it->second.is_integer() ||
      schema_it->second.as_integer() !=
          static_cast<std::int64_t>(companion_schema_version)) {
    return CompanionError{"$.schemaVersion",
                          "unsupported companion schemaVersion"};
  }

  const auto id_it = root.find("profileId");
  if (id_it == root.end() || !id_it->second.is_string() ||
      id_it->second.as_string() != base_profile.id) {
    return CompanionError{"$.profileId",
                          "companion profileId must match base profile id"};
  }

  CompanionProfile profile;
  profile.schema_version = companion_schema_version;
  profile.profile_id = base_profile.id;

  const auto slots_it = root.find("slots");
  if (slots_it != root.end()) {
    if (!slots_it->second.is_array()) {
      return CompanionError{"$.slots", "slots must be an array"};
    }
    const auto &arr = slots_it->second.as_array();
    if (arr.size() > max_companion_slots) {
      return CompanionError{"$.slots",
                            "companion slots exceed maximum limit (8)"};
    }
    for (std::size_t i = 0; i < arr.size(); ++i) {
      CompanionSlotMapping mapping;
      const std::string path = "$.slots[" + std::to_string(i) + "]";
      if (const auto error =
              parse_slot_mapping(arr[i], path, base_profile, mapping))
        return *error;
      profile.slots.push_back(std::move(mapping));
    }
  }

  if (const auto validation = validate_companion_profile(profile, base_profile))
    return *validation;
  return profile;
}
CompanionValidationResult
validate_companion_profile(const CompanionProfile &profile,
                           const GameProfile &base_profile) {
  if (profile.schema_version != companion_schema_version)
    return CompanionError{"$.schemaVersion",
                          "unsupported companion schemaVersion"};
  if (profile.profile_id.empty() || profile.profile_id != base_profile.id)
    return CompanionError{"$.profileId",
                          "companion profileId must match base profile id"};
  if (profile.slots.size() > max_companion_slots)
    return CompanionError{"$.slots",
                          "companion slots exceed maximum limit (8)"};
  std::vector<std::uint32_t> seen_slots;
  std::vector<std::string> seen_panels;
  for (std::size_t index = 0; index < profile.slots.size(); ++index) {
    const auto &slot = profile.slots[index];
    const std::string path = "$.slots[" + std::to_string(index) + "]";
    if (auto error = validate_slot_mapping(slot, base_profile, path))
      return error;
    if (std::ranges::find(seen_slots, slot.slot_index) != seen_slots.end())
      return CompanionError{path + ".slotIndex", "duplicate slotIndex mapping"};
    seen_slots.push_back(slot.slot_index);
    if (std::ranges::find(seen_panels, slot.panel_id) != seen_panels.end())
      return CompanionError{path + ".panelId",
                            "a panel may be assigned to only one slot"};
    seen_panels.push_back(slot.panel_id);
  }
  return std::nullopt;
}

std::string serialize_companion_profile_json(const CompanionProfile &profile) {
  std::string out = "{\"schemaVersion\":";
  out += std::to_string(profile.schema_version);
  out += ",\"profileId\":";
  json::append_quoted(out, profile.profile_id);
  out += ",\"slots\":[";
  json::Join join_slots;
  for (const auto &slot : profile.slots) {
    join_slots(out);
    out += "{\"slotIndex\":";
    out += std::to_string(slot.slot_index);
    out += ",\"panelId\":";
    json::append_quoted(out, slot.panel_id);
    out += ",\"pinned\":";
    json::append_bool(out, slot.pinned);
    out += ",\"clickThrough\":";
    json::append_bool(out, slot.click_through);
    out += ",\"opacity\":";
    json::append_number(out, slot.opacity);
    out += "}";
  }
  out += "]}";
  return out;
}

CompanionValidationResult
validate_companion_snapshot(const CompanionSessionSnapshot &snapshot) {
  if (snapshot.profile_id.empty() ||
      snapshot.profile_id.size() > max_companion_text_bytes)
    return CompanionError{"$.profileId",
                          "profileId must be a bounded non-empty string"};
  if (snapshot.session_key.empty() ||
      snapshot.session_key.size() > max_companion_text_bytes)
    return CompanionError{"$.sessionKey",
                          "sessionKey must be a bounded non-empty string"};
  if (snapshot.panels.size() > max_companion_snapshot_panels)
    return CompanionError{"$.panels", "panel count exceeds its bound"};
  if (snapshot.available_panels.size() > max_companion_available_panels)
    return CompanionError{"$.availablePanels",
                          "available panel count exceeds its bound"};
  if (snapshot.preferences.size() > max_companion_slots)
    return CompanionError{"$.preferences",
                          "preference count exceeds its bound"};
  std::vector<std::string_view> available_ids;
  available_ids.reserve(snapshot.available_panels.size());
  for (std::size_t panel_index = 0;
       panel_index < snapshot.available_panels.size(); ++panel_index) {
    const auto &panel = snapshot.available_panels[panel_index];
    const std::string path =
        "$.availablePanels[" + std::to_string(panel_index) + "]";
    if (panel.panel_id.empty() ||
        panel.panel_id.size() > max_companion_text_bytes)
      return CompanionError{path + ".panelId",
                            "panelId must be a bounded non-empty string"};
    if (panel.name.empty() || panel.name.size() > max_companion_text_bytes)
      return CompanionError{path + ".name",
                            "name must be a bounded non-empty string"};
    if (std::ranges::find(available_ids, panel.panel_id) != available_ids.end())
      return CompanionError{path + ".panelId", "duplicate available panelId"};
    available_ids.push_back(panel.panel_id);
  }
  std::vector<std::uint32_t> preference_slots;
  std::vector<std::string_view> preference_panels;
  for (std::size_t preference_index = 0;
       preference_index < snapshot.preferences.size(); ++preference_index) {
    const auto &preference = snapshot.preferences[preference_index];
    const std::string path =
        "$.preferences[" + std::to_string(preference_index) + "]";
    if (preference.slot_index >= max_companion_slots)
      return CompanionError{path + ".slotIndex",
                            "slotIndex must be between 0 and 7"};
    if (std::ranges::find(preference_slots, preference.slot_index) !=
        preference_slots.end())
      return CompanionError{path + ".slotIndex",
                            "duplicate preference slotIndex"};
    if (preference.panel_id.empty() ||
        preference.panel_id.size() > max_companion_text_bytes ||
        std::ranges::find(available_ids, preference.panel_id) ==
            available_ids.end())
      return CompanionError{path + ".panelId",
                            "panelId must name one available bounded panel"};
    if (std::ranges::find(preference_panels, preference.panel_id) !=
        preference_panels.end())
      return CompanionError{path + ".panelId",
                            "duplicate preference panelId"};
    if (!std::isfinite(preference.opacity) || preference.opacity < 0.0 ||
        preference.opacity > 1.0)
      return CompanionError{path + ".opacity",
                            "opacity must be finite and within [0,1]"};
    preference_slots.push_back(preference.slot_index);
    preference_panels.push_back(preference.panel_id);
  }
  std::vector<std::uint32_t> slots;
  for (std::size_t panel_index = 0; panel_index < snapshot.panels.size();
       ++panel_index) {
    const auto &panel = snapshot.panels[panel_index];
    const std::string path = "$.panels[" + std::to_string(panel_index) + "]";
    if (panel.panel_id.empty() ||
        panel.panel_id.size() > max_companion_text_bytes)
      return CompanionError{path + ".panelId",
                            "panelId must be a bounded non-empty string"};
    if (panel.slot_index >= max_companion_slots)
      return CompanionError{path + ".slotIndex",
                            "slotIndex must be between 0 and 7"};
    const auto preference = std::ranges::find(
        snapshot.preferences, panel.panel_id, &CompanionSlotMapping::panel_id);
    if (preference == snapshot.preferences.end() ||
        preference->slot_index != panel.slot_index)
      return CompanionError{path + ".slotIndex",
                            "panel slot must match its declared preference"};
    if (std::ranges::find(slots, panel.slot_index) != slots.end())
      return CompanionError{path + ".slotIndex",
                            "duplicate slotIndex in snapshot"};
    slots.push_back(panel.slot_index);
    if (panel.items.size() > max_companion_snapshot_items)
      return CompanionError{path + ".items", "item count exceeds its bound"};
    for (std::size_t item_index = 0; item_index < panel.items.size();
         ++item_index) {
      const auto &item = panel.items[item_index];
      const std::string item_path =
          path + ".items[" + std::to_string(item_index) + "]";
      for (const auto &field : std::array<BoundedField, 4>{{
               {"cooldownId", item.cooldown_id, false},
               {"name", item.name, false},
               {"icon", item.icon, true},
               {"color", item.color, true},
           }}) {
        if (auto error = bounded_field_error(item_path, field))
          return *error;
      }
      if (!std::isfinite(item.progress) || item.progress < 0.0 ||
          item.progress > 1.0)
        return CompanionError{item_path + ".progress",
                              "progress must be finite and within [0,1]"};
      if (item.max_charges == 0 || item.available_charges > item.max_charges)
        return CompanionError{item_path + ".availableCharges",
                              "charges must be within maxCharges"};
      if (item.remaining_ms < 0)
        return CompanionError{item_path + ".remainingMs",
                              "remainingMs cannot be negative"};
    }
  }
  return std::nullopt;
}

CompanionSerializeResult
serialize_companion_snapshot_json(const CompanionSessionSnapshot &snapshot) {
  if (const auto error = validate_companion_snapshot(snapshot))
    return *error;

  std::string out = "{\"profileId\":";
  json::append_quoted(out, snapshot.profile_id);
  out += ",\"sessionKey\":";
  json::append_quoted(out, snapshot.session_key);
  out += ",\"timestampMs\":";
  json::append_unsigned(out, snapshot.timestamp_ms);
  out += ",\"panels\":[";
  json::Join join_panels;
  for (const auto &panel : snapshot.panels) {
    join_panels(out);
    out += "{\"panelId\":";
    json::append_quoted(out, panel.panel_id);
    out += ",\"slotIndex\":";
    json::append_unsigned(out, panel.slot_index);
    out += ",\"visible\":";
    json::append_bool(out, panel.visible);
    out += ",\"items\":[";
    for (std::size_t it_idx = 0; it_idx < panel.items.size(); ++it_idx) {
      if (it_idx > 0)
        out += ",";
      const auto &item = panel.items[it_idx];
      out += "{\"cooldownId\":";
      json::append_quoted(out, item.cooldown_id);
      out += ",\"name\":";
      json::append_quoted(out, item.name);
      out += ",\"icon\":";
      json::append_quoted(out, item.icon);
      out += ",\"progress\":";
      json::append_number(out, item.progress);
      out += ",\"availableCharges\":";
      json::append_unsigned(out, item.available_charges);
      out += ",\"maxCharges\":";
      json::append_unsigned(out, item.max_charges);
      out += ",\"remainingMs\":";
      json::append_number(out, static_cast<double>(item.remaining_ms));
      out += ",\"isReady\":";
      json::append_bool(out, item.is_ready);
      out += ",\"color\":";
      json::append_quoted(out, item.color);
      out += "}";
    }
    out += "]}";
  }
  out += "],\"availablePanels\":[";
  for (std::size_t index = 0; index < snapshot.available_panels.size();
       ++index) {
    if (index > 0)
      out += ",";
    const auto &panel = snapshot.available_panels[index];
    out += "{\"panelId\":";
    json::append_quoted(out, panel.panel_id);
    out += ",\"name\":";
    json::append_quoted(out, panel.name);
    out += "}";
  }
  out += "],\"preferences\":[";
  for (std::size_t index = 0; index < snapshot.preferences.size(); ++index) {
    if (index > 0)
      out += ",";
    const auto &preference = snapshot.preferences[index];
    out += "{\"slotIndex\":";
    json::append_unsigned(out, preference.slot_index);
    out += ",\"panelId\":";
    json::append_quoted(out, preference.panel_id);
    out += ",\"pinned\":";
    json::append_bool(out, preference.pinned);
    out += ",\"clickThrough\":";
    json::append_bool(out, preference.click_through);
    out += ",\"opacity\":";
    json::append_number(out, preference.opacity);
    out += "}";
  }
  out += "]}";
  if (out.size() > max_companion_snapshot_bytes)
    return CompanionError{"$", "serialized companion snapshot exceeds its byte bound"};
  return out;
}

std::variant<CompanionSessionSnapshot, CompanionError>
parse_companion_snapshot_json(const std::string_view json_str) {
  if (json_str.size() > max_companion_snapshot_bytes)
    return CompanionError{"$",
                          "companion snapshot exceeds bounded message size"};
  const auto parsed_result = json::parse(json_str);
  const auto *parsed = std::get_if<json::Value>(&parsed_result);
  if (parsed == nullptr || !parsed->is_object())
    return CompanionError{"$",
                          "invalid JSON root object for companion snapshot"};
  const auto &root = parsed->as_object();

  CompanionSessionSnapshot snapshot;
  if (const auto error =
          required_string(*parsed, "profileId", "$", false, snapshot.profile_id))
    return *error;
  if (const auto error =
          required_string(*parsed, "sessionKey", "$", false, snapshot.session_key))
    return *error;
  std::int64_t timestamp{};
  if (const auto error =
          required_integer(*parsed, "timestampMs", "$", 0,
                           std::numeric_limits<std::int64_t>::max(), timestamp))
    return *error;
  snapshot.timestamp_ms = static_cast<std::uint64_t>(timestamp);

  const auto panels_it = root.find("panels");
  if (panels_it == root.end() || !panels_it->second.is_array())
    return CompanionError{"$.panels", "required field must be an array"};
  if (panels_it->second.as_array().size() > max_companion_snapshot_panels)
    return CompanionError{"$.panels", "panel count exceeds its bound"};

  for (std::size_t panel_index = 0;
       panel_index < panels_it->second.as_array().size(); ++panel_index) {
    const auto &panel_value = panels_it->second.as_array()[panel_index];
    const std::string path = "$.panels[" + std::to_string(panel_index) + "]";
    if (!panel_value.is_object())
      return CompanionError{path, "panel must be an object"};
    const auto &panel_object = panel_value.as_object();
    CompanionPanelSnapshot panel;
    if (const auto error = required_string(panel_value, "panelId", path, false,
                                           panel.panel_id))
      return *error;
    std::int64_t slot{};
    if (const auto error = required_integer(
            panel_value, "slotIndex", path, 0,
            static_cast<std::int64_t>(max_companion_slots - 1), slot))
      return *error;
    panel.slot_index = static_cast<std::uint32_t>(slot);
    if (const auto error =
            required_bool(panel_value, "visible", path, panel.visible))
      return *error;
    const auto items_it = panel_object.find("items");
    if (items_it == panel_object.end() || !items_it->second.is_array())
      return CompanionError{path + ".items", "required field must be an array"};
    if (items_it->second.as_array().size() > max_companion_snapshot_items)
      return CompanionError{path + ".items", "item count exceeds its bound"};

    for (std::size_t item_index = 0;
         item_index < items_it->second.as_array().size(); ++item_index) {
      const auto &item_value = items_it->second.as_array()[item_index];
      const std::string item_path =
          path + ".items[" + std::to_string(item_index) + "]";
      if (!item_value.is_object())
        return CompanionError{item_path, "item must be an object"};
      const auto &item_object = item_value.as_object();
      CompanionSnapshotItem item;
      if (const auto error = required_string(
              item_value, "cooldownId", item_path, false, item.cooldown_id))
        return *error;
      if (const auto error =
              required_string(item_value, "name", item_path, false, item.name))
        return *error;
      if (const auto error =
              required_string(item_value, "icon", item_path, true, item.icon))
        return *error;
      const auto progress_it = item_object.find("progress");
      if (progress_it == item_object.end() ||
          !progress_it->second.is_number() ||
          !std::isfinite(progress_it->second.as_number()) ||
          progress_it->second.as_number() < 0.0 ||
          progress_it->second.as_number() > 1.0)
        return CompanionError{item_path + ".progress",
                              "progress must be finite and within [0,1]"};
      item.progress = progress_it->second.as_number();
      std::int64_t available{};
      if (const auto error = required_integer(
              item_value, "availableCharges", item_path, 0,
              std::numeric_limits<std::uint32_t>::max(), available))
        return *error;
      item.available_charges = static_cast<std::uint32_t>(available);
      std::int64_t maximum{};
      if (const auto error = required_integer(
              item_value, "maxCharges", item_path, 1,
              std::numeric_limits<std::uint32_t>::max(), maximum))
        return *error;
      item.max_charges = static_cast<std::uint32_t>(maximum);
      if (item.available_charges > item.max_charges)
        return CompanionError{item_path + ".availableCharges",
                              "available charges exceed max charges"};
      std::int64_t remaining{};
      if (const auto error = required_integer(
              item_value, "remainingMs", item_path, 0,
              std::numeric_limits<std::int64_t>::max(), remaining))
        return *error;
      item.remaining_ms = remaining;
      if (const auto error =
              required_bool(item_value, "isReady", item_path, item.is_ready))
        return *error;
      if (const auto error = required_string(item_value, "color", item_path,
                                             true, item.color))
        return *error;
      panel.items.push_back(std::move(item));
    }
    snapshot.panels.push_back(std::move(panel));
  }
  const auto available_it = root.find("availablePanels");
  if (available_it == root.end() || !available_it->second.is_array())
    return CompanionError{"$.availablePanels",
                          "required field must be an array"};
  if (available_it->second.as_array().size() > max_companion_available_panels)
    return CompanionError{"$.availablePanels",
                          "available panel count exceeds its bound"};
  for (std::size_t panel_index = 0;
       panel_index < available_it->second.as_array().size(); ++panel_index) {
    const auto &panel_value = available_it->second.as_array()[panel_index];
    const std::string path =
        "$.availablePanels[" + std::to_string(panel_index) + "]";
    if (!panel_value.is_object())
      return CompanionError{path, "available panel must be an object"};
    CompanionAvailablePanel panel;
    if (const auto error = required_string(panel_value, "panelId",
                                           path, false, panel.panel_id))
      return *error;
    if (const auto error = required_string(panel_value, "name",
                                           path, false, panel.name))
      return *error;
    snapshot.available_panels.push_back(std::move(panel));
  }

  const auto preferences_it = root.find("preferences");
  if (preferences_it == root.end() || !preferences_it->second.is_array())
    return CompanionError{"$.preferences", "required field must be an array"};
  if (preferences_it->second.as_array().size() > max_companion_slots)
    return CompanionError{"$.preferences",
                          "preference count exceeds its bound"};
  for (std::size_t preference_index = 0;
       preference_index < preferences_it->second.as_array().size();
       ++preference_index) {
    const auto &preference_value =
        preferences_it->second.as_array()[preference_index];
    const std::string path =
        "$.preferences[" + std::to_string(preference_index) + "]";
    if (!preference_value.is_object())
      return CompanionError{path, "preference must be an object"};
    const auto &object = preference_value.as_object();
    CompanionSlotMapping preference;
    std::int64_t slot{};
    if (const auto error = required_integer(
            preference_value, "slotIndex", path, 0,
            static_cast<std::int64_t>(max_companion_slots - 1), slot))
      return *error;
    preference.slot_index = static_cast<std::uint32_t>(slot);
    if (const auto error = required_string(preference_value, "panelId", path, false,
                                           preference.panel_id))
      return *error;
    if (const auto error =
            required_bool(preference_value, "pinned", path, preference.pinned))
      return *error;
    if (const auto error = required_bool(preference_value, "clickThrough", path,
                                         preference.click_through))
      return *error;
    const auto opacity_it = object.find("opacity");
    if (opacity_it == object.end() || !opacity_it->second.is_number() ||
        !std::isfinite(opacity_it->second.as_number()) ||
        opacity_it->second.as_number() < 0.0 ||
        opacity_it->second.as_number() > 1.0)
      return CompanionError{path + ".opacity",
                            "opacity must be finite and within [0,1]"};
    preference.opacity = opacity_it->second.as_number();
    snapshot.preferences.push_back(std::move(preference));
  }
  // Enforce the same invariants as the validator (cross-panel duplicate
  // slots, bounds) so a parsed snapshot is never rejected on round-trip.
  if (const auto error = validate_companion_snapshot(snapshot))
    return *error;
  return snapshot;
}

} // namespace layerc::core
