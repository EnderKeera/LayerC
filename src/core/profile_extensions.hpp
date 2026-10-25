#pragma once

#include "layerc/core/json.hpp"
#include "profile_keys.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace layerc::core::detail {

enum class ProfileObject {
  root, game, module, cooldown, group, hid, device, control, panel, bar, binding, token
};

// Every classification comes from the object-scoped field declarations in
// profile_keys.hpp; there is no separately maintained name inventory.
inline std::span<const std::string_view> known_profile_fields(const ProfileObject kind) {
  switch (kind) {
  case ProfileObject::root: return profile_keys::root::names;
  case ProfileObject::game: return profile_keys::game::names;
  case ProfileObject::module: return profile_keys::module::names;
  case ProfileObject::cooldown: return profile_keys::cooldown::names;
  case ProfileObject::group: return profile_keys::group::names;
  case ProfileObject::hid: return profile_keys::hid::names;
  case ProfileObject::device: return profile_keys::device::names;
  case ProfileObject::control: return profile_keys::control::names;
  case ProfileObject::panel: return profile_keys::panel::names;
  case ProfileObject::bar: return profile_keys::bar::names;
  case ProfileObject::binding: return profile_keys::binding::names;
  case ProfileObject::token: return profile_keys::token::names;
  }
  return {};
}

inline json::Value::Object unknown_profile_fields(const json::Value &source,
                                                 const ProfileObject kind) {
  json::Value::Object result;
  const auto known = known_profile_fields(kind);
  for (const auto &[key, value] : source.as_object())
    if (std::ranges::find(known, key) == known.end()) result.emplace(key, value);
  return result;
}

// Never silently override or discard a known-field collision introduced by
// an invalid in-memory document. Parsed opaque fields cannot contain one.
inline json::Value with_profile_extensions(json::Value::Object fields,
                                           const json::Value::Object &extensions,
                                           const ProfileObject kind) {
  const auto known = known_profile_fields(kind);
  for (const auto &[key, value] : extensions) {
    if (std::ranges::find(known, key) != known.end())
      throw std::invalid_argument("profile extension overlaps a known field: " + key);
    if (!fields.emplace(key, value).second)
      throw std::invalid_argument("profile extension overlaps a known field: " + key);
  }
  return json::Value(std::move(fields));
}

} // namespace layerc::core::detail
