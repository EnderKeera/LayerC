#pragma once

#include "layerc/core/json.hpp"
#include "layerc/core/profile.hpp"
#include "profile_keys.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace layerc::core::detail {

enum class ProfileObject {
  root, game, module, cooldown, group, hid, device, control, panel, bar, binding, token, item
};

// Every classification comes from the object-scoped field declarations in
// profile_keys.hpp; there is no separately maintained name inventory.
template <typename Names, typename Filtered>
inline std::span<const std::string_view> schema_field_names(
    const Names &names, const Filtered &schema4_names,
    const std::uint32_t schema) noexcept {
  if (schema >= profile_schema_version) return names;
  return schema4_names.view();
}

inline std::span<const std::string_view>
known_profile_fields(const ProfileObject kind,
                     const std::uint32_t schema = profile_schema_version) {
  switch (kind) {
  case ProfileObject::root: return schema_field_names(
      profile_keys::root::names, profile_keys::root::schema4_names, schema);
  case ProfileObject::game: return schema_field_names(
      profile_keys::game::names, profile_keys::game::schema4_names, schema);
  case ProfileObject::module: return schema_field_names(
      profile_keys::module::names, profile_keys::module::schema4_names, schema);
  case ProfileObject::cooldown: return schema_field_names(
      profile_keys::cooldown::names, profile_keys::cooldown::schema4_names, schema);
  case ProfileObject::group: return schema_field_names(
      profile_keys::group::names, profile_keys::group::schema4_names, schema);
  case ProfileObject::hid: return schema_field_names(
      profile_keys::hid::names, profile_keys::hid::schema4_names, schema);
  case ProfileObject::device: return schema_field_names(
      profile_keys::device::names, profile_keys::device::schema4_names, schema);
  case ProfileObject::control: return schema_field_names(
      profile_keys::control::names, profile_keys::control::schema4_names, schema);
  case ProfileObject::panel: return schema_field_names(
      profile_keys::panel::names, profile_keys::panel::schema4_names, schema);
  case ProfileObject::bar: return schema_field_names(
      profile_keys::bar::names, profile_keys::bar::schema4_names, schema);
  case ProfileObject::binding: return schema_field_names(
      profile_keys::binding::names, profile_keys::binding::schema4_names, schema);
  case ProfileObject::token: return schema_field_names(
      profile_keys::token::names, profile_keys::token::schema4_names, schema);
  case ProfileObject::item: return schema_field_names(
      profile_keys::item::names, profile_keys::item::schema4_names, schema);
  }
  return {};
}

inline json::Value::Object unknown_profile_fields(
    const json::Value &source, const ProfileObject kind,
    const std::uint32_t schema = profile_schema_version) {
  json::Value::Object result;
  const auto known = known_profile_fields(kind, schema);
  for (const auto &[key, value] : source.as_object())
    if (std::ranges::find(known, key) == known.end()) result.emplace(key, value);
  return result;
}

// Never silently override or discard a known-field collision introduced by
// an invalid in-memory document. Parsed opaque fields cannot contain one.
inline json::Value with_profile_extensions(
    json::Value::Object fields, const json::Value::Object &extensions,
    const ProfileObject kind, const std::uint32_t schema = profile_schema_version) {
  const auto known = known_profile_fields(kind, schema);
  for (const auto &[key, value] : extensions) {
    if (std::ranges::find(known, key) != known.end())
      throw std::invalid_argument("profile extension overlaps a known field: " + key);
    if (!fields.emplace(key, value).second)
      throw std::invalid_argument("profile extension overlaps a known field: " + key);
  }
  return json::Value(std::move(fields));
}

} // namespace layerc::core::detail
