#include "layerc/core/companion.hpp"
#include "layerc/core/json.hpp"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <limits>
#include <tuple>

namespace layerc::core {

namespace {

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
  const auto slot_idx = slot_it->second.as_integer();
  if (slot_idx < 0 || static_cast<std::size_t>(slot_idx) >= max_companion_slots) {
    out_error = {path + ".slotIndex", "slotIndex must be between 0 and 7"};
    return false;
  }
  out_slot.slot_index = static_cast<std::uint32_t>(slot_idx);

  const auto panel_it = obj.find("panelId");
  if (panel_it == obj.end() || !panel_it->second.is_string()) {
    out_error = {path + ".panelId", "panelId must be a string"};
    return false;
  }
  out_slot.panel_id = panel_it->second.as_string();
  if (out_slot.panel_id.empty() ||
      out_slot.panel_id.size() > max_companion_text_bytes) {
    out_error = {path + ".panelId", "panelId must be a bounded non-empty string"};
    return false;
  }

  const bool panel_exists = std::ranges::any_of(
      base_profile.panels,
      [&](const Panel &p) { return p.id == out_slot.panel_id; });
  if (!panel_exists) {
    out_error = {path + ".panelId", "referenced panelId does not exist in base profile"};
    return false;
  }

  if (const auto pin_it = obj.find("pinned"); pin_it != obj.end()) {
    if (!pin_it->second.is_bool()) {
      out_error = {path + ".pinned", "pinned must be a boolean"};
      return false;
    }
    out_slot.pinned = pin_it->second.as_bool();
  }
  if (const auto ct_it = obj.find("clickThrough"); ct_it != obj.end()) {
    if (!ct_it->second.is_bool()) {
      out_error = {path + ".clickThrough", "clickThrough must be a boolean"};
      return false;
    }
    out_slot.click_through = ct_it->second.as_bool();
  }
  if (const auto op_it = obj.find("opacity"); op_it != obj.end()) {
    if (!op_it->second.is_number() ||
        !std::isfinite(op_it->second.as_number()) ||
        op_it->second.as_number() < 0.0 || op_it->second.as_number() > 1.0) {
      out_error = {path + ".opacity", "opacity must be finite and within [0,1]"};
      return false;
    }
    out_slot.opacity = op_it->second.as_number();
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
    return CompanionError{"$", "invalid JSON root object for companion profile"};
  }
  const auto &root = parsed->as_object();
  const auto schema_it = root.find("schemaVersion");
  if (schema_it == root.end() || !schema_it->second.is_integer() ||
      schema_it->second.as_integer() != static_cast<std::int64_t>(companion_schema_version)) {
    return CompanionError{"$.schemaVersion", "unsupported companion schemaVersion"};
  }

  const auto id_it = root.find("profileId");
  if (id_it == root.end() || !id_it->second.is_string() ||
      id_it->second.as_string() != base_profile.id) {
    return CompanionError{"$.profileId", "companion profileId must match base profile id"};
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
      return CompanionError{"$.slots", "companion slots exceed maximum limit (8)"};
    }
    std::vector<std::uint32_t> seen_slots;
    for (std::size_t i = 0; i < arr.size(); ++i) {
      CompanionSlotMapping mapping;
      CompanionError err;
      const std::string path = "$.slots[" + std::to_string(i) + "]";
      if (!parse_slot_mapping(arr[i], path, base_profile, mapping, err)) {
        return err;
      }
      if (std::ranges::find(seen_slots, mapping.slot_index) != seen_slots.end()) {
        return CompanionError{path + ".slotIndex", "duplicate slotIndex mapping"};
      }
      seen_slots.push_back(mapping.slot_index);
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
    if (slot.slot_index >= max_companion_slots)
      return {path + ".slotIndex", "slotIndex must be between 0 and 7"};
    if (std::ranges::find(seen_slots, slot.slot_index) != seen_slots.end())
      return {path + ".slotIndex", "duplicate slotIndex mapping"};
    seen_slots.push_back(slot.slot_index);
    if (slot.panel_id.empty() || slot.panel_id.size() > max_companion_text_bytes)
      return {path + ".panelId", "panelId must be a bounded non-empty string"};
    if (std::ranges::find(seen_panels, slot.panel_id) != seen_panels.end())
      return {path + ".panelId", "a panel may be assigned to only one slot"};
    seen_panels.push_back(slot.panel_id);
    const bool panel_exists = std::ranges::any_of(
        base_profile.panels,
        [&](const Panel &panel) { return panel.id == slot.panel_id; });
    if (!panel_exists)
      return {path + ".panelId", "referenced panelId does not exist in base profile"};
    if (!std::isfinite(slot.opacity) || slot.opacity < 0.0 || slot.opacity > 1.0)
      return {path + ".opacity", "opacity must be finite and within [0,1]"};
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
    if (i > 0) out += ",";
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
    out += std::to_string(slot.opacity);
    out += "}";
  }
  out += "]}";
  return out;
}

CompanionError validate_companion_snapshot(
    const CompanionSessionSnapshot &snapshot) {
  if (snapshot.profile_id.empty() ||
      snapshot.profile_id.size() > max_companion_text_bytes)
    return {"$.profileId", "profileId must be a bounded non-empty string"};
  if (snapshot.session_key.empty() ||
      snapshot.session_key.size() > max_companion_text_bytes)
    return {"$.sessionKey", "sessionKey must be a bounded non-empty string"};
  if (snapshot.panels.size() > max_companion_snapshot_panels)
    return {"$.panels", "panel count exceeds its bound"};
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
      const auto bounded_text = [&](std::string_view value,
                                    std::string_view field,
                                    bool allow_empty) -> CompanionError {
        if ((!allow_empty && value.empty()) ||
            value.size() > max_companion_text_bytes)
          return {item_path + "." + std::string(field),
                  "string is empty or exceeds its bound"};
        return {};
      };
      for (const auto &[value, field, allow_empty] :
           {std::tuple<std::string_view, std::string_view, bool>{
                item.cooldown_id, "cooldownId", false},
            {item.name, "name", false}, {item.icon, "icon", true},
            {item.color, "color", true}}) {
        if (const auto error = bounded_text(value, field, allow_empty);
            !error.message.empty())
          return error;
      }
      if (!std::isfinite(item.progress) || item.progress < 0.0 ||
          item.progress > 1.0)
        return {item_path + ".progress",
                "progress must be finite and within [0,1]"};
      if (item.max_charges == 0 ||
          item.available_charges > item.max_charges)
        return {item_path + ".availableCharges",
                "charges must be within maxCharges"};
      if (item.remaining_ms < 0)
        return {item_path + ".remainingMs", "remainingMs cannot be negative"};
    }
  }
  return {};
}

std::string
serialize_companion_snapshot_json(const CompanionSessionSnapshot &snapshot) {
  if (const auto error = validate_companion_snapshot(snapshot);
      !error.message.empty())
    return {};

  std::string out = "{\"profileId\":";
  json::append_quoted(out, snapshot.profile_id);
  out += ",\"sessionKey\":";
  json::append_quoted(out, snapshot.session_key);
  out += ",\"timestampMs\":";
  json::append_unsigned(out, snapshot.timestamp_ms);
  out += ",\"panels\":[";
  for (std::size_t p = 0; p < snapshot.panels.size(); ++p) {
    if (p > 0) out += ",";
    const auto &panel = snapshot.panels[p];
    out += "{\"panelId\":";
    json::append_quoted(out, panel.panel_id);
    out += ",\"slotIndex\":";
    json::append_unsigned(out, panel.slot_index);
    out += ",\"visible\":";
    out += panel.visible ? "true" : "false";
    out += ",\"items\":[";
    for (std::size_t it_idx = 0; it_idx < panel.items.size(); ++it_idx) {
      if (it_idx > 0) out += ",";
      const auto &item = panel.items[it_idx];
      out += "{\"cooldownId\":";
      json::append_quoted(out, item.cooldown_id);
      out += ",\"name\":";
      json::append_quoted(out, item.name);
      out += ",\"icon\":";
      json::append_quoted(out, item.icon);
      out += ",\"progress\":";
      out += std::to_string(item.progress);
      out += ",\"availableCharges\":";
      json::append_unsigned(out, item.available_charges);
      out += ",\"maxCharges\":";
      json::append_unsigned(out, item.max_charges);
      out += ",\"remainingMs\":";
      out += std::to_string(item.remaining_ms);
      out += ",\"isReady\":";
      out += item.is_ready ? "true" : "false";
      out += ",\"color\":";
      json::append_quoted(out, item.color);
      out += "}";
    }
    out += "]}";
  }
  out += "]}";
  return out;
}

std::variant<CompanionSessionSnapshot, CompanionError>
parse_companion_snapshot_json(const std::string_view json_str) {
  if (json_str.size() > max_companion_snapshot_bytes)
    return CompanionError{"$", "companion snapshot exceeds bounded message size"};
  const auto parsed_result = json::parse(json_str);
  const auto *parsed = std::get_if<json::Value>(&parsed_result);
  if (parsed == nullptr || !parsed->is_object())
    return CompanionError{"$", "invalid JSON root object for companion snapshot"};
  const auto &root = parsed->as_object();

  const auto required_string = [](const json::Value::Object &object,
                                  const char *key, const std::string &path,
                                  bool allow_empty, std::string &out)
      -> std::optional<CompanionError> {
    const auto it = object.find(key);
    if (it == object.end() || !it->second.is_string())
      return CompanionError{path + "." + key, "required field must be a string"};
    const auto &value = it->second.as_string();
    if ((!allow_empty && value.empty()) || value.size() > max_companion_text_bytes)
      return CompanionError{path + "." + key, "string is empty or exceeds its bound"};
    out = value;
    return std::nullopt;
  };
  const auto required_integer = [](const json::Value::Object &object,
                                   const char *key, const std::string &path,
                                   std::int64_t minimum, std::int64_t maximum,
                                   std::int64_t &out) -> std::optional<CompanionError> {
    const auto it = object.find(key);
    if (it == object.end() || !it->second.is_integer())
      return CompanionError{path + "." + key, "required field must be an exact integer"};
    const auto value = it->second.as_integer();
    if (value < minimum || value > maximum)
      return CompanionError{path + "." + key, "integer is outside its allowed range"};
    out = value;
    return std::nullopt;
  };
  const auto required_bool = [](const json::Value::Object &object,
                                const char *key, const std::string &path,
                                bool &out) -> std::optional<CompanionError> {
    const auto it = object.find(key);
    if (it == object.end() || !it->second.is_bool())
      return CompanionError{path + "." + key, "required field must be a boolean"};
    out = it->second.as_bool();
    return std::nullopt;
  };

  CompanionSessionSnapshot snapshot;
  if (const auto error = required_string(root, "profileId", "$", false, snapshot.profile_id)) return *error;
  if (const auto error = required_string(root, "sessionKey", "$", false, snapshot.session_key)) return *error;
  std::int64_t timestamp{};
  if (const auto error = required_integer(root, "timestampMs", "$", 0, std::numeric_limits<std::int64_t>::max(), timestamp)) return *error;
  snapshot.timestamp_ms = static_cast<std::uint64_t>(timestamp);

  const auto panels_it = root.find("panels");
  if (panels_it == root.end() || !panels_it->second.is_array())
    return CompanionError{"$.panels", "required field must be an array"};
  if (panels_it->second.as_array().size() > max_companion_snapshot_panels)
    return CompanionError{"$.panels", "panel count exceeds its bound"};

  for (std::size_t panel_index = 0; panel_index < panels_it->second.as_array().size(); ++panel_index) {
    const auto &panel_value = panels_it->second.as_array()[panel_index];
    const std::string path = "$.panels[" + std::to_string(panel_index) + "]";
    if (!panel_value.is_object()) return CompanionError{path, "panel must be an object"};
    const auto &panel_object = panel_value.as_object();
    CompanionPanelSnapshot panel;
    if (const auto error = required_string(panel_object, "panelId", path, false, panel.panel_id)) return *error;
    std::int64_t slot{};
    if (const auto error = required_integer(panel_object, "slotIndex", path, 0, static_cast<std::int64_t>(max_companion_slots - 1), slot)) return *error;
    panel.slot_index = static_cast<std::uint32_t>(slot);
    if (const auto error = required_bool(panel_object, "visible", path, panel.visible)) return *error;
    const auto items_it = panel_object.find("items");
    if (items_it == panel_object.end() || !items_it->second.is_array())
      return CompanionError{path + ".items", "required field must be an array"};
    if (items_it->second.as_array().size() > max_companion_snapshot_items)
      return CompanionError{path + ".items", "item count exceeds its bound"};

    for (std::size_t item_index = 0; item_index < items_it->second.as_array().size(); ++item_index) {
      const auto &item_value = items_it->second.as_array()[item_index];
      const std::string item_path = path + ".items[" + std::to_string(item_index) + "]";
      if (!item_value.is_object()) return CompanionError{item_path, "item must be an object"};
      const auto &item_object = item_value.as_object();
      CompanionSnapshotItem item;
      if (const auto error = required_string(item_object, "cooldownId", item_path, false, item.cooldown_id)) return *error;
      if (const auto error = required_string(item_object, "name", item_path, false, item.name)) return *error;
      if (const auto error = required_string(item_object, "icon", item_path, true, item.icon)) return *error;
      const auto progress_it = item_object.find("progress");
      if (progress_it == item_object.end() || !progress_it->second.is_number() || !std::isfinite(progress_it->second.as_number()) || progress_it->second.as_number() < 0.0 || progress_it->second.as_number() > 1.0)
        return CompanionError{item_path + ".progress", "progress must be finite and within [0,1]"};
      item.progress = progress_it->second.as_number();
      std::int64_t available{};
      if (const auto error = required_integer(item_object, "availableCharges", item_path, 0, std::numeric_limits<std::uint32_t>::max(), available)) return *error;
      item.available_charges = static_cast<std::uint32_t>(available);
      std::int64_t maximum{};
      if (const auto error = required_integer(item_object, "maxCharges", item_path, 1, std::numeric_limits<std::uint32_t>::max(), maximum)) return *error;
      item.max_charges = static_cast<std::uint32_t>(maximum);
      if (item.available_charges > item.max_charges) return CompanionError{item_path + ".availableCharges", "available charges exceed max charges"};
      std::int64_t remaining{};
      if (const auto error = required_integer(item_object, "remainingMs", item_path, 0, std::numeric_limits<std::int64_t>::max(), remaining)) return *error;
      item.remaining_ms = remaining;
      if (const auto error = required_bool(item_object, "isReady", item_path, item.is_ready)) return *error;
      if (const auto error = required_string(item_object, "color", item_path, true, item.color)) return *error;
      panel.items.push_back(std::move(item));
    }
    snapshot.panels.push_back(std::move(panel));
  }
  return snapshot;
}

} // namespace layerc::core
