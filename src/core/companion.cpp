#include "layerc/core/companion.hpp"
#include "layerc/core/json.hpp"

#include <algorithm>
#include <charconv>

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
  if (slot_it == obj.end() || !slot_it->second.is_number()) {
    out_error = {path + ".slotIndex", "slotIndex must be an integer"};
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
    if (!op_it->second.is_number()) {
      out_error = {path + ".opacity", "opacity must be a number between 0.0 and 1.0"};
      return false;
    }
    out_slot.opacity = std::clamp(op_it->second.as_number(), 0.0, 1.0);
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
  if (schema_it == root.end() || !schema_it->second.is_number() ||
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

  return profile;
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

std::string
serialize_companion_snapshot_json(const CompanionSessionSnapshot &snapshot) {
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
  auto parsed_result = json::parse(json_str);
  const auto *parsed = std::get_if<json::Value>(&parsed_result);
  if (parsed == nullptr || !parsed->is_object()) {
    return CompanionError{"$", "invalid JSON root object for companion snapshot"};
  }
  const auto &root = parsed->as_object();
  CompanionSessionSnapshot snapshot;
  if (const auto id_it = root.find("profileId"); id_it != root.end() && id_it->second.is_string()) {
    snapshot.profile_id = id_it->second.as_string();
  }
  if (const auto key_it = root.find("sessionKey"); key_it != root.end() && key_it->second.is_string()) {
    snapshot.session_key = key_it->second.as_string();
  }
  if (const auto ts_it = root.find("timestampMs"); ts_it != root.end() && ts_it->second.is_number()) {
    snapshot.timestamp_ms = static_cast<std::uint64_t>(ts_it->second.as_integer());
  }

  if (const auto panels_it = root.find("panels"); panels_it != root.end() && panels_it->second.is_array()) {
    for (const auto &p_val : panels_it->second.as_array()) {
      if (!p_val.is_object()) continue;
      const auto &p_obj = p_val.as_object();
      CompanionPanelSnapshot p_snap;
      if (const auto pid = p_obj.find("panelId"); pid != p_obj.end() && pid->second.is_string()) {
        p_snap.panel_id = pid->second.as_string();
      }
      if (const auto s_idx = p_obj.find("slotIndex"); s_idx != p_obj.end() && s_idx->second.is_number()) {
        p_snap.slot_index = static_cast<std::uint32_t>(s_idx->second.as_integer());
      }
      if (const auto vis = p_obj.find("visible"); vis != p_obj.end() && vis->second.is_bool()) {
        p_snap.visible = vis->second.as_bool();
      }
      if (const auto items_it = p_obj.find("items"); items_it != p_obj.end() && items_it->second.is_array()) {
        for (const auto &it_val : items_it->second.as_array()) {
          if (!it_val.is_object()) continue;
          const auto &it_obj = it_val.as_object();
          CompanionSnapshotItem item;
          if (const auto cid = it_obj.find("cooldownId"); cid != it_obj.end() && cid->second.is_string()) item.cooldown_id = cid->second.as_string();
          if (const auto nm = it_obj.find("name"); nm != it_obj.end() && nm->second.is_string()) item.name = nm->second.as_string();
          if (const auto ic = it_obj.find("icon"); ic != it_obj.end() && ic->second.is_string()) item.icon = ic->second.as_string();
          if (const auto pr = it_obj.find("progress"); pr != it_obj.end() && pr->second.is_number()) item.progress = pr->second.as_number();
          if (const auto ac = it_obj.find("availableCharges"); ac != it_obj.end() && ac->second.is_number()) item.available_charges = static_cast<std::uint32_t>(ac->second.as_integer());
          if (const auto mc = it_obj.find("maxCharges"); mc != it_obj.end() && mc->second.is_number()) item.max_charges = static_cast<std::uint32_t>(mc->second.as_integer());
          if (const auto rm = it_obj.find("remainingMs"); rm != it_obj.end() && rm->second.is_number()) item.remaining_ms = rm->second.as_integer();
          if (const auto rdy = it_obj.find("isReady"); rdy != it_obj.end() && rdy->second.is_bool()) item.is_ready = rdy->second.as_bool();
          if (const auto clr = it_obj.find("color"); clr != it_obj.end() && clr->second.is_string()) item.color = clr->second.as_string();
          p_snap.items.push_back(std::move(item));
        }
      }
      snapshot.panels.push_back(std::move(p_snap));
    }
  }
  return snapshot;
}

} // namespace layerc::core
