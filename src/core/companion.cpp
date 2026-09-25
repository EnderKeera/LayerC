#include "layerc/core/companion.hpp"
#include "layerc/core/json_reader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace layerc::core {

namespace {
// Every companion string shares one bound; the message names the field so all
// validators report the same diagnostic for the same failure.
CompanionValidationResult bounded_text(const std::string &path,
                                       const std::string_view key,
                                       const std::string_view value,
                                       const bool allow_empty) {
  if ((!allow_empty && value.empty()) || value.size() > max_companion_text_bytes)
    return CompanionError{path + "." + std::string(key),
                          std::string(key) +
                              (allow_empty ? " must be a bounded string"
                                           : " must be a bounded non-empty string")};
  return std::nullopt;
}

CompanionError reader_error(const json::Reader &reader) {
  return {reader.error().path, reader.error().message};
}

// Companion text and integer-token policies are wire requirements; the
// generic type/range checks and path diagnostics live in json::Reader. One
// reader spans each document, so its first failure is converted exactly once.
bool read_text(json::Reader &reader, const json::Value &object,
               const std::string_view key, const std::string &path,
               const bool allow_empty, std::string &out) {
  return reader.string(object, key, path, out, json::Required::yes,
                       allow_empty ? json::EmptyString::allow
                                   : json::EmptyString::reject,
                       max_companion_text_bytes);
}
template <typename Integer>
bool read_count(json::Reader &reader, const json::Value &object,
                const std::string_view key, const std::string &path,
                const std::int64_t minimum, const std::int64_t maximum,
                Integer &out) {
  const auto value = reader.integer(object, key, path, minimum, maximum,
                                    json::Required::yes, 0,
                                    json::IntegerEncoding::integer_token);
  if (value) out = static_cast<Integer>(*value);
  return value.has_value();
}
bool read_unit(json::Reader &reader, const json::Value &object,
               const std::string_view key, const std::string &path,
               double &out) {
  const auto value =
      reader.number(object, key, path, 0.0, 1.0, json::Required::yes);
  if (value) out = *value;
  return value.has_value();
}

CompanionValidationResult
validate_slot_mapping(const CompanionSlotMapping &slot,
                      const GameProfile &base_profile,
                      const std::string &path) {
  if (slot.slot_index >= max_companion_slots)
    return CompanionError{path + ".slotIndex",
                          "slotIndex must be between 0 and 7"};
  if (auto error = bounded_text(path, "panelId", slot.panel_id, false))
    return error;
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

// Shape and type only; validate_companion_profile owns the slot invariants.
bool parse_slot_mapping(json::Reader &fields, const json::Value &value,
                        const std::string &path, CompanionSlotMapping &slot) {
  const auto index = fields.integer(value, "slotIndex", path, 0,
                                    max_companion_slots - 1, json::Required::yes,
                                    0, json::IntegerEncoding::integer_token);
  const auto opacity = fields.number(value, "opacity", path, 0.0, 1.0,
                                     json::Required::no, slot.opacity);
  if (!index || !opacity ||
      !read_text(fields, value, "panelId", path, false, slot.panel_id) ||
      !fields.boolean(value, "pinned", path, slot.pinned, json::Required::no) ||
      !fields.boolean(value, "clickThrough", path, slot.click_through,
                      json::Required::no))
    return false;
  slot.slot_index = static_cast<std::uint32_t>(*index);
  slot.opacity = *opacity;
  return true;
}

} // namespace

std::variant<CompanionProfile, CompanionError>
parse_companion_profile_json(const std::string_view json_str,
                             const GameProfile &base_profile) {
  if (json_str.size() > max_companion_sidecar_bytes) {
    return CompanionError{"$", "companion sidecar exceeds bounded size limit"};
  }
  const auto parsed = json::parse_bounded(json_str, max_companion_sidecar_bytes);
  if (const auto *error = std::get_if<json::FieldError>(&parsed))
    return CompanionError{error->path, error->message};
  return parse_companion_profile_value(std::get<json::Value>(parsed), base_profile);
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

  json::Reader fields;
  const auto *slots = fields.array(value, "slots", "$", json::Required::no);
  if (!fields.ok()) return reader_error(fields);
  if (slots != nullptr) {
    if (slots->size() > max_companion_slots)
      return CompanionError{"$.slots",
                            "companion slots exceed maximum limit (8)"};
    fields.each(*slots, "$.slots",
                [&](const json::Value &entry, const std::string &path) {
                  CompanionSlotMapping mapping;
                  if (parse_slot_mapping(fields, entry, path, mapping))
                    profile.slots.push_back(std::move(mapping));
                });
    if (!fields.ok()) return reader_error(fields);
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
validate_available_panels(const std::vector<CompanionAvailablePanel> &panels,
                          std::vector<std::string_view> &available_ids) {
  available_ids.reserve(panels.size());
  for (std::size_t panel_index = 0; panel_index < panels.size(); ++panel_index) {
    const auto &panel = panels[panel_index];
    const std::string path =
        "$.availablePanels[" + std::to_string(panel_index) + "]";
    if (auto error = bounded_text(path, "panelId", panel.panel_id, false))
      return error;
    if (auto error = bounded_text(path, "name", panel.name, false))
      return error;
    if (std::ranges::find(available_ids, panel.panel_id) != available_ids.end())
      return CompanionError{path + ".panelId", "duplicate available panelId"};
    available_ids.push_back(panel.panel_id);
  }
  return std::nullopt;
}

CompanionValidationResult validate_preferences(
    const std::vector<CompanionSlotMapping> &preferences,
    const std::vector<std::string_view> &available_ids) {
  std::vector<std::uint32_t> preference_slots;
  std::vector<std::string_view> preference_panels;
  for (std::size_t preference_index = 0;
       preference_index < preferences.size(); ++preference_index) {
    const auto &preference = preferences[preference_index];
    const std::string path =
        "$.preferences[" + std::to_string(preference_index) + "]";
    if (preference.slot_index >= max_companion_slots)
      return CompanionError{path + ".slotIndex",
                            "slotIndex must be between 0 and 7"};
    if (std::ranges::find(preference_slots, preference.slot_index) !=
        preference_slots.end())
      return CompanionError{path + ".slotIndex",
                            "duplicate preference slotIndex"};
    // Available ids are already bounded and non-empty, so membership is the
    // whole text check.
    if (std::ranges::find(available_ids, preference.panel_id) ==
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
  return std::nullopt;
}

CompanionValidationResult
validate_snapshot_item(const CompanionSnapshotItem &item,
                       const std::string &item_path) {
  if (auto error = bounded_text(item_path, "cooldownId", item.cooldown_id, false))
    return error;
  if (auto error = bounded_text(item_path, "name", item.name, false))
    return error;
  if (auto error = bounded_text(item_path, "icon", item.icon, true))
    return error;
  if (auto error = bounded_text(item_path, "color", item.color, true))
    return error;
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
  return std::nullopt;
}

CompanionValidationResult
validate_snapshot_panels(const std::vector<CompanionPanelSnapshot> &panels,
                         const std::vector<CompanionSlotMapping> &preferences) {
  std::vector<std::uint32_t> slots;
  for (std::size_t panel_index = 0; panel_index < panels.size(); ++panel_index) {
    const auto &panel = panels[panel_index];
    const std::string path = "$.panels[" + std::to_string(panel_index) + "]";
    if (auto error = bounded_text(path, "panelId", panel.panel_id, false))
      return error;
    if (panel.slot_index >= max_companion_slots)
      return CompanionError{path + ".slotIndex",
                            "slotIndex must be between 0 and 7"};
    const auto preference = std::ranges::find(
        preferences, panel.panel_id, &CompanionSlotMapping::panel_id);
    if (preference == preferences.end() ||
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
      const auto item_path =
          path + ".items[" + std::to_string(item_index) + "]";
      if (auto error = validate_snapshot_item(panel.items[item_index], item_path))
        return error;
    }
  }
  return std::nullopt;
}

CompanionValidationResult
validate_companion_snapshot(const CompanionSessionSnapshot &snapshot) {
  if (auto error = bounded_text("$", "profileId", snapshot.profile_id, false))
    return error;
  if (auto error = bounded_text("$", "sessionKey", snapshot.session_key, false))
    return error;
  if (snapshot.panels.size() > max_companion_snapshot_panels)
    return CompanionError{"$.panels", "panel count exceeds its bound"};
  if (snapshot.available_panels.size() > max_companion_available_panels)
    return CompanionError{"$.availablePanels",
                          "available panel count exceeds its bound"};
  if (snapshot.preferences.size() > max_companion_slots)
    return CompanionError{"$.preferences",
                          "preference count exceeds its bound"};
  std::vector<std::string_view> available_ids;
  if (auto error =
          validate_available_panels(snapshot.available_panels, available_ids))
    return error;
  if (auto error = validate_preferences(snapshot.preferences, available_ids))
    return error;
  return validate_snapshot_panels(snapshot.panels, snapshot.preferences);
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
    json::Join join_items;
    for (const auto &item : panel.items) {
      join_items(out);
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
  json::Join join_available;
  for (const auto &panel : snapshot.available_panels) {
    join_available(out);
    out += "{\"panelId\":";
    json::append_quoted(out, panel.panel_id);
    out += ",\"name\":";
    json::append_quoted(out, panel.name);
    out += "}";
  }
  out += "],\"preferences\":[";
  json::Join join_preferences;
  for (const auto &preference : snapshot.preferences) {
    join_preferences(out);
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

namespace {

// Reads the required array `key`, enforcing `maximum` before parsing each
// element into `out` with the document's shared reader.
template <typename Item, typename Parse>
bool read_bounded_array(json::Reader &reader, const json::Value &object,
                        const std::string_view key, const std::string &path,
                        const std::size_t maximum,
                        const std::string_view bound_message, Parse parse,
                        std::vector<Item> &out) {
  const auto *values = reader.array(object, key, path, json::Required::yes);
  if (values == nullptr) return false;
  const auto field_path = path + "." + std::string(key);
  if (values->size() > maximum)
    return reader.fail(field_path, std::string(bound_message));
  return reader.each(*values, field_path,
                     [&](const json::Value &value, const std::string &item_path) {
                       Item item;
                       if (parse(reader, value, item_path, item))
                         out.push_back(std::move(item));
                     });
}

bool parse_snapshot_item(json::Reader &reader, const json::Value &value,
                         const std::string &path, CompanionSnapshotItem &item) {
  if (!value.is_object()) return reader.fail(path, "item must be an object");
  return read_text(reader, value, "cooldownId", path, false, item.cooldown_id) &&
         read_text(reader, value, "name", path, false, item.name) &&
         read_text(reader, value, "icon", path, true, item.icon) &&
         read_unit(reader, value, "progress", path, item.progress) &&
         read_count(reader, value, "availableCharges", path, 0,
                    std::numeric_limits<std::uint32_t>::max(),
                    item.available_charges) &&
         read_count(reader, value, "maxCharges", path, 1,
                    std::numeric_limits<std::uint32_t>::max(),
                    item.max_charges) &&
         read_count(reader, value, "remainingMs", path, 0,
                    std::numeric_limits<std::int64_t>::max(),
                    item.remaining_ms) &&
         reader.boolean(value, "isReady", path, item.is_ready,
                        json::Required::yes) &&
         read_text(reader, value, "color", path, true, item.color);
}

bool parse_snapshot_panel(json::Reader &reader, const json::Value &value,
                          const std::string &path,
                          CompanionPanelSnapshot &panel) {
  if (!value.is_object()) return reader.fail(path, "panel must be an object");
  return read_text(reader, value, "panelId", path, false, panel.panel_id) &&
         read_count(reader, value, "slotIndex", path, 0,
                    static_cast<std::int64_t>(max_companion_slots - 1),
                    panel.slot_index) &&
         reader.boolean(value, "visible", path, panel.visible,
                        json::Required::yes) &&
         read_bounded_array(reader, value, "items", path,
                            max_companion_snapshot_items,
                            "item count exceeds its bound", parse_snapshot_item,
                            panel.items);
}

bool parse_available_panel(json::Reader &reader, const json::Value &value,
                           const std::string &path,
                           CompanionAvailablePanel &panel) {
  if (!value.is_object())
    return reader.fail(path, "available panel must be an object");
  return read_text(reader, value, "panelId", path, false, panel.panel_id) &&
         read_text(reader, value, "name", path, false, panel.name);
}

bool parse_preference(json::Reader &reader, const json::Value &value,
                      const std::string &path,
                      CompanionSlotMapping &preference) {
  if (!value.is_object())
    return reader.fail(path, "preference must be an object");
  return read_count(reader, value, "slotIndex", path, 0,
                    static_cast<std::int64_t>(max_companion_slots - 1),
                    preference.slot_index) &&
         read_text(reader, value, "panelId", path, false, preference.panel_id) &&
         reader.boolean(value, "pinned", path, preference.pinned,
                        json::Required::yes) &&
         reader.boolean(value, "clickThrough", path, preference.click_through,
                        json::Required::yes) &&
         read_unit(reader, value, "opacity", path, preference.opacity);
}

} // namespace

std::variant<CompanionSessionSnapshot, CompanionError>
parse_companion_snapshot_json(const std::string_view json_str) {
  if (json_str.size() > max_companion_snapshot_bytes)
    return CompanionError{"$",
                          "companion snapshot exceeds bounded message size"};
  const auto parsed_result =
      json::parse_bounded(json_str, max_companion_snapshot_bytes);
  if (const auto *error = std::get_if<json::FieldError>(&parsed_result))
    return CompanionError{error->path, error->message};
  const auto &root = std::get<json::Value>(parsed_result);
  if (!root.is_object())
    return CompanionError{"$", "companion snapshot root must be an object"};

  CompanionSessionSnapshot snapshot;
  json::Reader reader;
  if (!read_text(reader, root, "profileId", "$", false, snapshot.profile_id) ||
      !read_text(reader, root, "sessionKey", "$", false, snapshot.session_key) ||
      !read_count(reader, root, "timestampMs", "$", 0,
                  std::numeric_limits<std::int64_t>::max(),
                  snapshot.timestamp_ms) ||
      !read_bounded_array(reader, root, "panels", "$",
                          max_companion_snapshot_panels,
                          "panel count exceeds its bound", parse_snapshot_panel,
                          snapshot.panels) ||
      !read_bounded_array(reader, root, "availablePanels", "$",
                          max_companion_available_panels,
                          "available panel count exceeds its bound",
                          parse_available_panel, snapshot.available_panels) ||
      !read_bounded_array(reader, root, "preferences", "$", max_companion_slots,
                          "preference count exceeds its bound",
                          parse_preference, snapshot.preferences))
    return reader_error(reader);
  // Enforce the same invariants as the validator (cross-panel duplicate
  // slots, bounds) so a parsed snapshot is never rejected on round-trip.
  if (const auto error = validate_companion_snapshot(snapshot))
    return *error;
  return snapshot;
}

} // namespace layerc::core
