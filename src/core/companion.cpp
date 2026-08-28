#include "layerc/core/companion.hpp"
#include "layerc/core/json.hpp"

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

std::optional<CompanionError> required_string(const json::Value::Object &object,
                                              const std::string_view key,
                                              const std::string &path,
                                              const bool allow_empty,
                                              std::string &out) {
  const auto it = object.find(key);
  if (it == object.end() || !it->second.is_string())
    return CompanionError{path + "." + std::string(key),
                          "required field must be a string"};
  const auto &value = it->second.as_string();
  if (const auto error = bounded_field_error(
          path, {.name = key, .value = value, .allow_empty = allow_empty}))
    return error;
  out = value;
  return std::nullopt;
}

std::optional<CompanionError>
required_integer(const json::Value::Object &object, const std::string_view key,
                 const std::string &path, const std::int64_t minimum,
                 const std::int64_t maximum, std::int64_t &out) {
  const auto it = object.find(key);
  if (it == object.end() || !it->second.is_integer())
    return CompanionError{path + "." + std::string(key),
                          "required field must be an exact integer"};
  const auto value = it->second.as_integer();
  if (value < minimum || value > maximum)
    return CompanionError{path + "." + std::string(key),
                          "integer is outside its allowed range"};
  out = value;
  return std::nullopt;
}

std::optional<CompanionError> required_bool(const json::Value::Object &object,
                                            const std::string_view key,
                                            const std::string &path,
                                            bool &out) {
  const auto it = object.find(key);
  if (it == object.end() || !it->second.is_bool())
    return CompanionError{path + "." + std::string(key),
                          "required field must be a boolean"};
  out = it->second.as_bool();
  return std::nullopt;
}

CompanionError validate_slot_mapping(const CompanionSlotMapping &slot,
                                     const GameProfile &base_profile,
                                     const std::string &path) {
  if (slot.slot_index >= max_companion_slots)
    return {path + ".slotIndex", "slotIndex must be between 0 and 7"};
  if (slot.panel_id.empty() || slot.panel_id.size() > max_companion_text_bytes)
    return {path + ".panelId", "panelId must be a bounded non-empty string"};
  const bool panel_exists =
      std::ranges::any_of(base_profile.panels, [&](const Panel &p) {
        return p.id == slot.panel_id;
      });
  if (!panel_exists)
    return {path + ".panelId",
            "referenced panelId does not exist in base profile"};
  if (!std::isfinite(slot.opacity) || slot.opacity < 0.0 || slot.opacity > 1.0)
    return {path + ".opacity", "opacity must be finite and within [0,1]"};
  return {};
}

bool parse_slot_mapping(const json::Value &val, const std::string &path,
                        const GameProfile &base_profile,
                        CompanionSlotMapping &out_slot,
                        CompanionError &out_error) {
  if (!val.is_object()) {
    out_error = {path, "slot mapping must be a JSON object"};
    return false;
  }
  const auto &obj = val.as_object();
  const auto slot_it = obj.find("slotIndex");
  if (slot_it == obj.end() || !slot_it->second.is_integer()) {
    out_error = {path + ".slotIndex", "slotIndex must be an exact integer"};
    return false;
  }
  const auto slot_index = slot_it->second.as_integer();
  if (slot_index < 0 ||
      slot_index > std::numeric_limits<std::uint32_t>::max()) {
    out_error = {path + ".slotIndex", "slotIndex must be between 0 and 7"};
    return false;
  }
  out_slot.slot_index = static_cast<std::uint32_t>(slot_index);

  const auto panel_it = obj.find("panelId");
  if (panel_it == obj.end() || !panel_it->second.is_string()) {
    out_error = {path + ".panelId", "panelId must be a string"};
    return false;
  }
  out_slot.panel_id = panel_it->second.as_string();

  if (const auto pin_it = obj.find("pinned"); pin_it != obj.end()) {
    if (!pin_it->second.is_bool()) {
      out_error = {path + ".pinned", "pinned must be a boolean"};
      return false;
    }
    out_slot.pinned = pin_it->second.as_bool();
  }
  if (const auto click_it = obj.find("clickThrough"); click_it != obj.end()) {
    if (!click_it->second.is_bool()) {
      out_error = {path + ".clickThrough", "clickThrough must be a boolean"};
      return false;
    }
    out_slot.click_through = click_it->second.as_bool();
  }
  if (const auto opacity_it = obj.find("opacity"); opacity_it != obj.end()) {
    if (!opacity_it->second.is_number()) {
      out_error = {path + ".opacity", "opacity must be a number"};
      return false;
    }
    out_slot.opacity = opacity_it->second.as_number();
  }
  if (auto error = validate_slot_mapping(out_slot, base_profile, path);
      !error.message.empty()) {
    out_error = std::move(error);
    return false;
  }
  return true;
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
  const auto &root = parsed->as_object();
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
      CompanionError err;
      const std::string path = "$.slots[" + std::to_string(i) + "]";
      if (!parse_slot_mapping(arr[i], path, base_profile, mapping, err)) {
        return err;
      }
      profile.slots.push_back(std::move(mapping));
    }
  }

  if (const auto validation = validate_companion_profile(profile, base_profile);
      !validation.message.empty())
    return validation;
  return profile;
}
CompanionError validate_companion_profile(const CompanionProfile &profile,
                                          const GameProfile &base_profile) {
  if (profile.schema_version != companion_schema_version)
    return {"$.schemaVersion", "unsupported companion schemaVersion"};
  if (profile.profile_id.empty() || profile.profile_id != base_profile.id)
    return {"$.profileId", "companion profileId must match base profile id"};
  if (profile.slots.size() > max_companion_slots)
    return {"$.slots", "companion slots exceed maximum limit (8)"};
  std::vector<std::uint32_t> seen_slots;
  std::vector<std::string> seen_panels;
  for (std::size_t index = 0; index < profile.slots.size(); ++index) {
    const auto &slot = profile.slots[index];
    const std::string path = "$.slots[" + std::to_string(index) + "]";
    if (auto err = validate_slot_mapping(slot, base_profile, path);
        !err.message.empty())
      return err;
    if (std::ranges::find(seen_slots, slot.slot_index) != seen_slots.end())
      return {path + ".slotIndex", "duplicate slotIndex mapping"};
    seen_slots.push_back(slot.slot_index);
    if (std::ranges::find(seen_panels, slot.panel_id) != seen_panels.end())
      return {path + ".panelId", "a panel may be assigned to only one slot"};
    seen_panels.push_back(slot.panel_id);
  }
  return {};
}

std::string serialize_companion_profile_json(const CompanionProfile &profile) {
  std::string out = "{\"schemaVersion\":";
  out += std::to_string(profile.schema_version);
  out += ",\"profileId\":";
  json::append_quoted(out, profile.profile_id);
  out += ",\"slots\":[";
  for (std::size_t i = 0; i < profile.slots.size(); ++i) {
    if (i > 0)
      out += ",";
    const auto &slot = profile.slots[i];
    out += "{\"slotIndex\":";
    out += std::to_string(slot.slot_index);
    out += ",\"panelId\":";
    json::append_quoted(out, slot.panel_id);
    out += ",\"pinned\":";
    out += slot.pinned ? "true" : "false";
    out += ",\"clickThrough\":";
    out += slot.click_through ? "true" : "false";
    out += ",\"opacity\":";
    json::append_number(out, slot.opacity);
    out += "}";
  }
  out += "]}";
  return out;
}

CompanionError
validate_companion_snapshot(const CompanionSessionSnapshot &snapshot) {
  if (snapshot.profile_id.empty() ||
      snapshot.profile_id.size() > max_companion_text_bytes)
    return {"$.profileId", "profileId must be a bounded non-empty string"};
  if (snapshot.session_key.empty() ||
      snapshot.session_key.size() > max_companion_text_bytes)
    return {"$.sessionKey", "sessionKey must be a bounded non-empty string"};
  if (snapshot.panels.size() > max_companion_snapshot_panels)
    return {"$.panels", "panel count exceeds its bound"};
  if (snapshot.available_panels.size() > max_companion_available_panels)
    return {"$.availablePanels", "available panel count exceeds its bound"};
  if (snapshot.preferences.size() > max_companion_slots)
    return {"$.preferences", "preference count exceeds its bound"};
  std::vector<std::string_view> available_ids;
  available_ids.reserve(snapshot.available_panels.size());
  for (std::size_t panel_index = 0;
       panel_index < snapshot.available_panels.size(); ++panel_index) {
    const auto &panel = snapshot.available_panels[panel_index];
    const std::string path =
        "$.availablePanels[" + std::to_string(panel_index) + "]";
    if (panel.panel_id.empty() ||
        panel.panel_id.size() > max_companion_text_bytes)
      return {path + ".panelId", "panelId must be a bounded non-empty string"};
    if (panel.name.empty() || panel.name.size() > max_companion_text_bytes)
      return {path + ".name", "name must be a bounded non-empty string"};
    if (std::ranges::find(available_ids, panel.panel_id) != available_ids.end())
      return {path + ".panelId", "duplicate available panelId"};
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
      return {path + ".slotIndex", "slotIndex must be between 0 and 7"};
    if (std::ranges::find(preference_slots, preference.slot_index) !=
        preference_slots.end())
      return {path + ".slotIndex", "duplicate preference slotIndex"};
    if (preference.panel_id.empty() ||
        preference.panel_id.size() > max_companion_text_bytes ||
        std::ranges::find(available_ids, preference.panel_id) ==
            available_ids.end())
      return {path + ".panelId",
              "panelId must name one available bounded panel"};
    if (std::ranges::find(preference_panels, preference.panel_id) !=
        preference_panels.end())
      return {path + ".panelId", "duplicate preference panelId"};
    if (!std::isfinite(preference.opacity) || preference.opacity < 0.0 ||
        preference.opacity > 1.0)
      return {path + ".opacity", "opacity must be finite and within [0,1]"};
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
      return {path + ".panelId", "panelId must be a bounded non-empty string"};
    if (panel.slot_index >= max_companion_slots)
      return {path + ".slotIndex", "slotIndex must be between 0 and 7"};
    const auto preference = std::ranges::find(
        snapshot.preferences, panel.panel_id, &CompanionSlotMapping::panel_id);
    if (preference == snapshot.preferences.end() ||
        preference->slot_index != panel.slot_index)
      return {path + ".slotIndex",
              "panel slot must match its declared preference"};
    if (std::ranges::find(slots, panel.slot_index) != slots.end())
      return {path + ".slotIndex", "duplicate slotIndex in snapshot"};
    slots.push_back(panel.slot_index);
    if (panel.items.size() > max_companion_snapshot_items)
      return {path + ".items", "item count exceeds its bound"};
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
        return {item_path + ".progress",
                "progress must be finite and within [0,1]"};
      if (item.max_charges == 0 || item.available_charges > item.max_charges)
        return {item_path + ".availableCharges",
                "charges must be within maxCharges"};
      if (item.remaining_ms < 0)
        return {item_path + ".remainingMs", "remainingMs cannot be negative"};
    }
  }
  return {};
}

std::optional<std::string>
serialize_companion_snapshot_json(const CompanionSessionSnapshot &snapshot) {
  if (const auto error = validate_companion_snapshot(snapshot);
      !error.message.empty())
    return std::nullopt;

  std::string out = "{\"profileId\":";
  json::append_quoted(out, snapshot.profile_id);
  out += ",\"sessionKey\":";
  json::append_quoted(out, snapshot.session_key);
  out += ",\"timestampMs\":";
  json::append_unsigned(out, snapshot.timestamp_ms);
  out += ",\"panels\":[";
  for (std::size_t p = 0; p < snapshot.panels.size(); ++p) {
    if (p > 0)
      out += ",";
    const auto &panel = snapshot.panels[p];
    out += "{\"panelId\":";
    json::append_quoted(out, panel.panel_id);
    out += ",\"slotIndex\":";
    json::append_unsigned(out, panel.slot_index);
    out += ",\"visible\":";
    out += panel.visible ? "true" : "false";
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
      out += item.is_ready ? "true" : "false";
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
    out += preference.pinned ? "true" : "false";
    out += ",\"clickThrough\":";
    out += preference.click_through ? "true" : "false";
    out += ",\"opacity\":";
    json::append_number(out, preference.opacity);
    out += "}";
  }
  out += "]}";
  if (out.size() > max_companion_snapshot_bytes)
    return std::nullopt;
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
          required_string(root, "profileId", "$", false, snapshot.profile_id))
    return *error;
  if (const auto error =
          required_string(root, "sessionKey", "$", false, snapshot.session_key))
    return *error;
  std::int64_t timestamp{};
  if (const auto error =
          required_integer(root, "timestampMs", "$", 0,
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
    if (const auto error = required_string(panel_object, "panelId", path, false,
                                           panel.panel_id))
      return *error;
    std::int64_t slot{};
    if (const auto error = required_integer(
            panel_object, "slotIndex", path, 0,
            static_cast<std::int64_t>(max_companion_slots - 1), slot))
      return *error;
    panel.slot_index = static_cast<std::uint32_t>(slot);
    if (const auto error =
            required_bool(panel_object, "visible", path, panel.visible))
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
              item_object, "cooldownId", item_path, false, item.cooldown_id))
        return *error;
      if (const auto error =
              required_string(item_object, "name", item_path, false, item.name))
        return *error;
      if (const auto error =
              required_string(item_object, "icon", item_path, true, item.icon))
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
              item_object, "availableCharges", item_path, 0,
              std::numeric_limits<std::uint32_t>::max(), available))
        return *error;
      item.available_charges = static_cast<std::uint32_t>(available);
      std::int64_t maximum{};
      if (const auto error = required_integer(
              item_object, "maxCharges", item_path, 1,
              std::numeric_limits<std::uint32_t>::max(), maximum))
        return *error;
      item.max_charges = static_cast<std::uint32_t>(maximum);
      if (item.available_charges > item.max_charges)
        return CompanionError{item_path + ".availableCharges",
                              "available charges exceed max charges"};
      std::int64_t remaining{};
      if (const auto error = required_integer(
              item_object, "remainingMs", item_path, 0,
              std::numeric_limits<std::int64_t>::max(), remaining))
        return *error;
      item.remaining_ms = remaining;
      if (const auto error =
              required_bool(item_object, "isReady", item_path, item.is_ready))
        return *error;
      if (const auto error = required_string(item_object, "color", item_path,
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
    if (const auto error = required_string(panel_value.as_object(), "panelId",
                                           path, false, panel.panel_id))
      return *error;
    if (const auto error = required_string(panel_value.as_object(), "name",
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
            object, "slotIndex", path, 0,
            static_cast<std::int64_t>(max_companion_slots - 1), slot))
      return *error;
    preference.slot_index = static_cast<std::uint32_t>(slot);
    if (const auto error = required_string(object, "panelId", path, false,
                                           preference.panel_id))
      return *error;
    if (const auto error =
            required_bool(object, "pinned", path, preference.pinned))
      return *error;
    if (const auto error = required_bool(object, "clickThrough", path,
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
  if (const auto error = validate_companion_snapshot(snapshot);
      !error.message.empty())
    return error;
  return snapshot;
}

} // namespace layerc::core
