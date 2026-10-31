#include "layerc/core/companion.hpp"
#include "layerc/core/json_reader.hpp"
#include "layerc/core/profile_wire.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

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

// Readers check shape, type, and representability only: integer tokens must
// fit the destination type. Semantic bounds (text length, non-empty IDs,
// [0,1] ranges, charge and slot limits, element counts) belong solely to
// validate_companion_profile/validate_companion_snapshot, so parsed and
// directly constructed documents get identical diagnostics. One reader spans
// each document, so its first failure is converted exactly once.
bool read_text(json::Reader &reader, const json::Value &object,
               const std::string_view key, const std::string &path,
               std::string &out) {
  return reader.string(object, key, path, out, json::Required::yes,
                       json::EmptyString::allow);
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
bool read_number(json::Reader &reader, const json::Value &object,
                 const std::string_view key, const std::string &path,
                 double &out) {
  const auto value = reader.number(object, key, path,
                                   std::numeric_limits<double>::lowest(),
                                   std::numeric_limits<double>::max(),
                                   json::Required::yes);
  if (value) out = *value;
  return value.has_value();
}

// The one slot-mapping rule set, shared by the companion sidecar and the
// Game Bar preferences; only panel membership differs (base-profile panels
// vs available panels). Membership implies a bounded non-empty panel ID.
template <typename PanelExists>
CompanionValidationResult
validate_slot_set(const std::vector<CompanionSlotMapping> &slots,
                  const std::string_view root, PanelExists &&panel_exists) {
  if (slots.size() > max_companion_slots)
    return CompanionError{std::string(root),
                          "companion slots exceed maximum limit (8)"};
  std::vector<std::uint32_t> seen_slots;
  std::vector<std::string_view> seen_panels;
  for (std::size_t index = 0; index < slots.size(); ++index) {
    const auto &slot = slots[index];
    const std::string path =
        std::string(root) + "[" + std::to_string(index) + "]";
    if (slot.slot_index >= max_companion_slots)
      return CompanionError{path + ".slotIndex",
                            "slotIndex must be between 0 and 7"};
    if (std::ranges::find(seen_slots, slot.slot_index) != seen_slots.end())
      return CompanionError{path + ".slotIndex", "duplicate slotIndex"};
    if (!panel_exists(slot.panel_id))
      return CompanionError{path + ".panelId",
                            "panelId must name an available panel"};
    if (std::ranges::find(seen_panels, slot.panel_id) != seen_panels.end())
      return CompanionError{path + ".panelId",
                            "a panel may be assigned to only one slot"};
    if (!std::isfinite(slot.opacity) || slot.opacity < 0.0 || slot.opacity > 1.0)
      return CompanionError{path + ".opacity",
                            "opacity must be finite and within [0,1]"};
    seen_slots.push_back(slot.slot_index);
    seen_panels.push_back(slot.panel_id);
  }
  return std::nullopt;
}

// Shape and type only; validate_companion_profile owns the slot invariants.
bool parse_slot_mapping(json::Reader &fields, const json::Value &value,
                        const std::string &path, CompanionSlotMapping &slot) {
  const auto index = fields.integer(
      value, "slotIndex", path, 0, std::numeric_limits<std::uint32_t>::max(),
      json::Required::yes, 0, json::IntegerEncoding::integer_token);
  const auto opacity =
      fields.number(value, "opacity", path, std::numeric_limits<double>::lowest(),
                    std::numeric_limits<double>::max(), json::Required::no,
                    slot.opacity);
  if (!index || !opacity ||
      !read_text(fields, value, "panelId", path, slot.panel_id) ||
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
  const auto parsed = json::parse_bounded(json_str, max_companion_sidecar_bytes);
  if (const auto *error = std::get_if<json::FieldError>(&parsed))
    return *error;
  return parse_companion_profile_value(std::get<json::Value>(parsed), base_profile);
}

std::variant<CompanionProfile, CompanionError>
parse_companion_profile_value(const json::Value &value,
                              const GameProfile &base_profile) {
  if (!value.is_object())
    return CompanionError{"$", "companion profile root must be an object"};
  // Shape only: a missing or mistyped header leaves a value that
  // validate_companion_profile rejects with the header's own message.
  CompanionProfile profile;
  const auto *schema = value.find("schemaVersion");
  const auto version = schema != nullptr && schema->is_integer()
                           ? schema->as_integer()
                           : std::int64_t{-1};
  profile.schema_version =
      version >= 0 && version <= std::numeric_limits<std::uint32_t>::max()
          ? static_cast<std::uint32_t>(version)
          : 0U;
  if (const auto *id = value.find("profileId"); id != nullptr && id->is_string())
    profile.profile_id = id->as_string();

  json::Reader fields;
  const auto *slots = fields.array(value, "slots", "$", json::Required::no);
  if (!fields.ok()) return reader_error(fields);
  if (slots != nullptr) {
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
  return validate_slot_set(
      profile.slots, "$.slots", [&](const std::string_view panel_id) {
        return std::ranges::any_of(base_profile.panels, [&](const Panel &panel) {
          return panel.id == panel_id;
        });
      });
}

namespace {

json::Value slot_mapping_value(const CompanionSlotMapping &slot) {
  return json::Value(json::Value::Object{
      {"slotIndex", json::Value(std::int64_t{slot.slot_index})},
      {"panelId", json::Value(slot.panel_id)},
      {"pinned", json::Value(slot.pinned)},
      {"clickThrough", json::Value(slot.click_through)},
      {"opacity", json::Value(slot.opacity)}});
}

json::Value slot_mappings_value(
    const std::vector<CompanionSlotMapping> &slots) {
  json::Value::Array values;
  values.reserve(slots.size());
  for (const auto &slot : slots)
    values.push_back(slot_mapping_value(slot));
  return json::Value(std::move(values));
}

} // namespace

std::string serialize_companion_profile_json(const CompanionProfile &profile) {
  const json::Value root(json::Value::Object{
      {"schemaVersion", json::Value(std::int64_t{profile.schema_version})},
      {"profileId", json::Value(profile.profile_id)},
      {"slots", slot_mappings_value(profile.slots)}});
  std::string out;
  json::append_value(out, root);
  return out;
}

namespace {

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

CompanionValidationResult
validate_snapshot_item(const CompanionSnapshotItem &item,
                       const std::string &item_path) {
  if (auto error = bounded_text(item_path, "cooldownId", item.cooldown_id, false))
    return error;
  if (auto error = bounded_text(item_path, "name", item.name, false))
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
validate_typed_item(const CompanionTypedItem &item, const std::string &item_path) {
  if (auto error = bounded_text(item_path, "itemId", item.item_id, false))
    return error;
  if (!enum_wire_name(profile_wire::overlay_item_kind, item.kind))
    return CompanionError{item_path + ".kind", "kind must be a known item kind"};
  if (auto error = bounded_text(item_path, "label", item.label, true))
    return error;
  if (auto error = bounded_text(item_path, "valueText", item.value_text, true))
    return error;
  if (!std::isfinite(item.fill) || item.fill < 0.0 || item.fill > 1.0)
    return CompanionError{item_path + ".fill", "fill must be finite and within [0,1]"};
  if (item.pips > max_counter_pips || item.filled_pips > item.pips)
    return CompanionError{item_path + ".pips",
                          "pips must be at most 8 and cover filledPips"};
  if (item.color > 0xFFFFFFU)
    return CompanionError{item_path + ".color", "color must be 0xRRGGBB"};
  return std::nullopt;
}

// Version 1 keeps its exact cooldown-only shape; version 2 carries only
// resolved typed rows, so neither wire can smuggle the other's fields.
CompanionValidationResult
validate_panel_rows(const CompanionPanelSnapshot &panel, const std::uint32_t version,
                    const std::string &path) {
  if (version == companion_snapshot_version_legacy) {
    if (!panel.typed_items.empty() || !panel.visible || panel.omitted_items != 0)
      return CompanionError{path, "typed panel fields require snapshotVersion 2"};
    if (panel.items.size() > max_companion_snapshot_items)
      return CompanionError{path + ".items", "item count exceeds its bound"};
    for (std::size_t index = 0; index < panel.items.size(); ++index)
      if (auto error = validate_snapshot_item(
              panel.items[index], path + ".items[" + std::to_string(index) + "]"))
        return error;
    return std::nullopt;
  }
  if (!panel.items.empty())
    return CompanionError{path + ".items", "version 2 panels carry typed rows only"};
  if (panel.typed_items.size() > max_companion_snapshot_items)
    return CompanionError{path + ".items", "item count exceeds its bound"};
  std::vector<std::string_view> ids;
  for (std::size_t index = 0; index < panel.typed_items.size(); ++index) {
    const auto &item = panel.typed_items[index];
    const auto item_path = path + ".items[" + std::to_string(index) + "]";
    if (auto error = validate_typed_item(item, item_path)) return error;
    if (std::ranges::find(ids, item.item_id) != ids.end())
      return CompanionError{item_path + ".itemId", "duplicate itemId in panel"};
    ids.push_back(item.item_id);
  }
  return std::nullopt;
}

CompanionValidationResult
validate_snapshot_panels(const std::vector<CompanionPanelSnapshot> &panels,
                         const std::vector<CompanionSlotMapping> &preferences,
                         const std::uint32_t version) {
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
    if (auto error = validate_panel_rows(panel, version, path)) return error;
  }
  return std::nullopt;
}

} // namespace

CompanionValidationResult
validate_companion_snapshot(const CompanionSessionSnapshot &snapshot) {
  if (snapshot.snapshot_version != companion_snapshot_version_legacy &&
      snapshot.snapshot_version != companion_snapshot_version_typed)
    return CompanionError{"$.snapshotVersion", "unsupported snapshotVersion"};
  if (auto error = bounded_text("$", "profileId", snapshot.profile_id, false))
    return error;
  if (auto error = bounded_text("$", "sessionKey", snapshot.session_key, false))
    return error;
  // Match the existing integer-token reader before narrowing into a Value.
  if (snapshot.timestamp_ms >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return CompanionError{"$.timestampMs", "timestampMs exceeds its integer bound"};
  if (snapshot.panels.size() > max_companion_snapshot_panels)
    return CompanionError{"$.panels", "panel count exceeds its bound"};
  if (snapshot.available_panels.size() > max_companion_available_panels)
    return CompanionError{"$.availablePanels",
                          "available panel count exceeds its bound"};
  std::vector<std::string_view> available_ids;
  if (auto error =
          validate_available_panels(snapshot.available_panels, available_ids))
    return error;
  if (auto error = validate_slot_set(
          snapshot.preferences, "$.preferences",
          [&](const std::string_view panel_id) {
            return std::ranges::find(available_ids, panel_id) !=
                   available_ids.end();
          }))
    return error;
  return validate_snapshot_panels(snapshot.panels, snapshot.preferences,
                                  snapshot.snapshot_version);
}

namespace {

json::Value typed_item_value(const CompanionTypedItem &item) {
  return json::Value(json::Value::Object{
      {"itemId", json::Value(item.item_id)},
      {"kind", json::Value(std::string(
                   *enum_wire_name(profile_wire::overlay_item_kind, item.kind)))},
      {"label", json::Value(item.label)},
      {"valueText", json::Value(item.value_text)},
      {"fill", json::Value(item.fill)},
      {"hasProgress", json::Value(item.has_progress)},
      {"pips", json::Value(std::int64_t{item.pips})},
      {"filledPips", json::Value(std::int64_t{item.filled_pips})},
      {"color", json::Value(std::int64_t{item.color})},
      {"active", json::Value(item.active)},
      {"flashOn", json::Value(item.flash_on)},
      {"unavailable", json::Value(item.unavailable)}});
}

json::Value legacy_item_value(const CompanionSnapshotItem &item) {
  return json::Value(json::Value::Object{
      {"cooldownId", json::Value(item.cooldown_id)},
      {"name", json::Value(item.name)},
      {"progress", json::Value(item.progress)},
      {"availableCharges", json::Value(std::int64_t{item.available_charges})},
      {"maxCharges", json::Value(std::int64_t{item.max_charges})},
      {"remainingMs", json::Value(item.remaining_ms)},
      {"isReady", json::Value(item.is_ready)}});
}

json::Value panel_value(const CompanionPanelSnapshot &panel, const bool typed) {
  json::Value::Array items;
  if (typed) {
    items.reserve(panel.typed_items.size());
    for (const auto &item : panel.typed_items) items.push_back(typed_item_value(item));
    return json::Value(json::Value::Object{
        {"panelId", json::Value(panel.panel_id)},
        {"slotIndex", json::Value(std::int64_t{panel.slot_index})},
        {"visible", json::Value(panel.visible)},
        {"omittedItems", json::Value(std::int64_t{panel.omitted_items})},
        {"items", json::Value(std::move(items))}});
  }
  items.reserve(panel.items.size());
  for (const auto &item : panel.items) items.push_back(legacy_item_value(item));
  return json::Value(json::Value::Object{
      {"panelId", json::Value(panel.panel_id)},
      {"slotIndex", json::Value(std::int64_t{panel.slot_index})},
      {"items", json::Value(std::move(items))}});
}

} // namespace

CompanionSerializeResult
serialize_companion_snapshot_json(const CompanionSessionSnapshot &snapshot) {
  if (const auto error = validate_companion_snapshot(snapshot))
    return *error;

  const bool typed = snapshot.snapshot_version == companion_snapshot_version_typed;
  json::Value::Array panels;
  panels.reserve(snapshot.panels.size());
  for (const auto &panel : snapshot.panels) panels.push_back(panel_value(panel, typed));
  json::Value::Array available_panels;
  available_panels.reserve(snapshot.available_panels.size());
  for (const auto &panel : snapshot.available_panels)
    available_panels.emplace_back(json::Value::Object{
        {"panelId", json::Value(panel.panel_id)},
        {"name", json::Value(panel.name)}});
  json::Value::Object fields{
      {"profileId", json::Value(snapshot.profile_id)},
      {"sessionKey", json::Value(snapshot.session_key)},
      {"timestampMs", json::Value(static_cast<std::int64_t>(snapshot.timestamp_ms))},
      {"panels", json::Value(std::move(panels))},
      {"availablePanels", json::Value(std::move(available_panels))},
      {"preferences", slot_mappings_value(snapshot.preferences)}};
  // Version 1 output stays byte-identical: it never names its version.
  if (typed)
    fields.emplace("snapshotVersion", json::Value(std::int64_t{snapshot.snapshot_version}));
  const json::Value root(std::move(fields));
  std::string out;
  json::append_value(out, root);
  if (out.size() > max_companion_snapshot_bytes)
    return CompanionError{"$", "serialized companion snapshot exceeds its byte bound"};
  return out;
}

namespace {

// Reads the required array `key`, parsing each element into `out` with the
// document's shared reader. Element counts are validator bounds; the byte
// bound already enforced by json::parse_bounded limits the work here.
template <typename Item, typename Parse>
bool read_array(json::Reader &reader, const json::Value &object,
                const std::string_view key, const std::string &path,
                Parse parse, std::vector<Item> &out) {
  const auto *values = reader.array(object, key, path, json::Required::yes);
  if (values == nullptr) return false;
  const auto field_path = path + "." + std::string(key);
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
  return read_text(reader, value, "cooldownId", path, item.cooldown_id) &&
         read_text(reader, value, "name", path, item.name) &&
         read_number(reader, value, "progress", path, item.progress) &&
         read_count(reader, value, "availableCharges", path, 0,
                    std::numeric_limits<std::uint32_t>::max(),
                    item.available_charges) &&
         read_count(reader, value, "maxCharges", path, 0,
                    std::numeric_limits<std::uint32_t>::max(),
                    item.max_charges) &&
         read_count(reader, value, "remainingMs", path,
                    std::numeric_limits<std::int64_t>::min(),
                    std::numeric_limits<std::int64_t>::max(),
                    item.remaining_ms) &&
         reader.boolean(value, "isReady", path, item.is_ready,
                        json::Required::yes);
}

bool parse_snapshot_panel(json::Reader &reader, const json::Value &value,
                          const std::string &path,
                          CompanionPanelSnapshot &panel) {
  if (!value.is_object()) return reader.fail(path, "panel must be an object");
  return read_text(reader, value, "panelId", path, panel.panel_id) &&
         read_count(reader, value, "slotIndex", path, 0,
                    std::numeric_limits<std::uint32_t>::max(),
                    panel.slot_index) &&
         read_array(reader, value, "items", path, parse_snapshot_item,
                    panel.items);
}

bool parse_typed_item(json::Reader &reader, const json::Value &value,
                      const std::string &path, CompanionTypedItem &item) {
  if (!value.is_object()) return reader.fail(path, "item must be an object");
  std::string kind;
  if (!read_text(reader, value, "itemId", path, item.item_id) ||
      !read_text(reader, value, "kind", path, kind))
    return false;
  // Unlike opaque profile items, an unknown wire kind rejects the snapshot.
  const auto parsed = parse_enum_wire(profile_wire::overlay_item_kind, kind);
  if (!parsed) return reader.fail(path + ".kind", "unknown item kind");
  item.kind = *parsed;
  return read_text(reader, value, "label", path, item.label) &&
         read_text(reader, value, "valueText", path, item.value_text) &&
         read_number(reader, value, "fill", path, item.fill) &&
         reader.boolean(value, "hasProgress", path, item.has_progress,
                        json::Required::yes) &&
         read_count(reader, value, "pips", path, 0,
                    std::numeric_limits<std::uint32_t>::max(), item.pips) &&
         read_count(reader, value, "filledPips", path, 0,
                    std::numeric_limits<std::uint32_t>::max(), item.filled_pips) &&
         read_count(reader, value, "color", path, 0,
                    std::numeric_limits<std::uint32_t>::max(), item.color) &&
         reader.boolean(value, "active", path, item.active, json::Required::yes) &&
         reader.boolean(value, "flashOn", path, item.flash_on, json::Required::yes) &&
         reader.boolean(value, "unavailable", path, item.unavailable,
                        json::Required::yes);
}

bool parse_typed_panel(json::Reader &reader, const json::Value &value,
                       const std::string &path, CompanionPanelSnapshot &panel) {
  if (!value.is_object()) return reader.fail(path, "panel must be an object");
  return read_text(reader, value, "panelId", path, panel.panel_id) &&
         read_count(reader, value, "slotIndex", path, 0,
                    std::numeric_limits<std::uint32_t>::max(), panel.slot_index) &&
         reader.boolean(value, "visible", path, panel.visible, json::Required::yes) &&
         read_count(reader, value, "omittedItems", path, 0,
                    std::numeric_limits<std::uint32_t>::max(), panel.omitted_items) &&
         read_array(reader, value, "items", path, parse_typed_item,
                    panel.typed_items);
}

bool parse_available_panel(json::Reader &reader, const json::Value &value,
                           const std::string &path,
                           CompanionAvailablePanel &panel) {
  if (!value.is_object())
    return reader.fail(path, "available panel must be an object");
  return read_text(reader, value, "panelId", path, panel.panel_id) &&
         read_text(reader, value, "name", path, panel.name);
}

bool parse_preference(json::Reader &reader, const json::Value &value,
                      const std::string &path,
                      CompanionSlotMapping &preference) {
  if (!value.is_object())
    return reader.fail(path, "preference must be an object");
  // Shape only: validate_companion_snapshot owns the slot rules.
  if (!read_count(reader, value, "slotIndex", path, 0,
                  std::numeric_limits<std::uint32_t>::max(),
                  preference.slot_index) ||
      !read_text(reader, value, "panelId", path, preference.panel_id) ||
      !reader.boolean(value, "pinned", path, preference.pinned,
                      json::Required::yes) ||
      !reader.boolean(value, "clickThrough", path, preference.click_through,
                      json::Required::yes))
    return false;
  const auto opacity = reader.number(
      value, "opacity", path, std::numeric_limits<double>::lowest(),
      std::numeric_limits<double>::max(), json::Required::yes);
  if (opacity) preference.opacity = *opacity;
  return opacity.has_value();
}

} // namespace

std::variant<CompanionSessionSnapshot, CompanionError>
parse_companion_snapshot_json(const std::string_view json_str) {
  const auto parsed_result =
      json::parse_bounded(json_str, max_companion_snapshot_bytes);
  if (const auto *error = std::get_if<json::FieldError>(&parsed_result))
    return *error;
  const auto &root = std::get<json::Value>(parsed_result);
  if (!root.is_object())
    return CompanionError{"$", "companion snapshot root must be an object"};

  CompanionSessionSnapshot snapshot;
  json::Reader reader;
  // An absent snapshotVersion is the cooldown-only version 1 wire.
  const auto version = reader.integer(
      root, "snapshotVersion", "$", 0, std::numeric_limits<std::uint32_t>::max(),
      json::Required::no, companion_snapshot_version_legacy,
      json::IntegerEncoding::integer_token);
  if (!version) return reader_error(reader);
  snapshot.snapshot_version = static_cast<std::uint32_t>(*version);
  if (snapshot.snapshot_version != companion_snapshot_version_legacy &&
      snapshot.snapshot_version != companion_snapshot_version_typed)
    return CompanionError{"$.snapshotVersion", "unsupported snapshotVersion"};
  const bool typed = snapshot.snapshot_version == companion_snapshot_version_typed;
  // timestampMs is unsigned on the wire; a negative token is unrepresentable.
  if (!read_text(reader, root, "profileId", "$", snapshot.profile_id) ||
      !read_text(reader, root, "sessionKey", "$", snapshot.session_key) ||
      !read_count(reader, root, "timestampMs", "$", 0,
                  std::numeric_limits<std::int64_t>::max(),
                  snapshot.timestamp_ms) ||
      !(typed ? read_array(reader, root, "panels", "$", parse_typed_panel,
                           snapshot.panels)
              : read_array(reader, root, "panels", "$", parse_snapshot_panel,
                           snapshot.panels)) ||
      !read_array(reader, root, "availablePanels", "$", parse_available_panel,
                  snapshot.available_panels) ||
      !read_array(reader, root, "preferences", "$", parse_preference,
                  snapshot.preferences))
    return reader_error(reader);
  // The validator is the sole owner of semantic bounds, so a parsed snapshot
  // and a directly constructed one are judged identically.
  if (const auto error = validate_companion_snapshot(snapshot))
    return *error;
  return snapshot;
}

} // namespace layerc::core
