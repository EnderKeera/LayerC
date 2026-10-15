#pragma once

#include "layerc/core/profile.hpp"
#include "layerc/core/utf8.hpp"
#include "profile_extensions.hpp"

#include <cmath>

namespace layerc::core::detail {

inline std::optional<ProfileError> validate_opaque_value(
    const json::Value &value, const std::string &path, const std::size_t depth) {
  if ((value.is_array() || value.is_object()) && depth >= json::maximum_nesting_depth)
    return ProfileError{path, "maximum nesting depth exceeded"};
  if (value.is_string() && !is_valid_utf8(value.as_string()))
    return ProfileError{path, "expected valid UTF-8"};
  if (value.is_number() && !value.is_integer() && !std::isfinite(value.as_number()))
    return ProfileError{path, "expected finite number"};
  if (value.is_array()) {
    for (std::size_t i{}; i < value.as_array().size(); ++i)
      if (auto error = validate_opaque_value(value.as_array()[i],
              path + "[" + std::to_string(i) + "]", depth + 1)) return error;
  } else if (value.is_object()) {
    for (const auto &[key, child] : value.as_object()) {
      if (!is_valid_utf8(key)) return ProfileError{path, "expected valid UTF-8 key"};
      if (auto error = validate_opaque_value(child, path + "." + key, depth + 1))
        return error;
    }
  }
  return std::nullopt;
}

inline std::optional<ProfileError> validate_extensions(
    const json::Value::Object &fields, const ProfileObject kind,
    const std::string &path, const std::size_t depth) {
  const auto known = known_profile_fields(kind);
  for (const auto &[key, value] : fields) {
    if (std::ranges::find(known, key) != known.end())
      return ProfileError{path + "." + key, "opaque field overlaps a known field"};
    if (!is_valid_utf8(key)) return ProfileError{path, "expected valid UTF-8 key"};
    if (auto error = validate_opaque_value(value, path + "." + key, depth)) return error;
  }
  return std::nullopt;
}

inline std::optional<ProfileError> validate_profile_extensions(const GameProfile &p) {
  if (auto error = validate_extensions(p.extra_fields, ProfileObject::root, "$", 1))
    return error;
  if (auto error = validate_extensions(p.game.extra_fields, ProfileObject::game, "$.game", 2))
    return error;
  const auto collection = [] (const auto &items, const ProfileObject kind,
                              const std::string &path) -> std::optional<ProfileError> {
    for (std::size_t i{}; i < items.size(); ++i)
      if (auto error = validate_extensions(items[i].extra_fields, kind,
              path + "[" + std::to_string(i) + "]", 3)) return error;
    return std::nullopt;
  };
  if (auto error = collection(p.modules, ProfileObject::module, "$.modules")) return error;
  if (auto error = collection(p.cooldowns, ProfileObject::cooldown, "$.cooldowns")) return error;
  if (auto error = collection(p.cooldown_groups, ProfileObject::group, "$.cooldownGroups")) return error;
  if (auto error = collection(p.hid_bindings, ProfileObject::hid, "$.hidBindings")) return error;
  if (auto error = collection(p.panels, ProfileObject::panel, "$.panels")) return error;
  if (auto error = collection(p.bindings, ProfileObject::binding, "$.bindings")) return error;
  for (std::size_t i{}; i < p.panels.size(); ++i)
    if (auto error = validate_extensions(p.panels[i].bar.extra_fields, ProfileObject::bar,
            "$.panels[" + std::to_string(i) + "].bar", 4)) return error;
  for (std::size_t i{}; i < p.hid_bindings.size(); ++i) {
    const auto &binding = p.hid_bindings[i];
    const auto path = "$.hidBindings[" + std::to_string(i) + "]";
    if (auto error = validate_extensions(binding.device_extra_fields,
            ProfileObject::device, path + ".device", 4)) return error;
    if (auto error = validate_extensions(binding.control_extra_fields,
            ProfileObject::control, path + ".control", 4)) return error;
  }
  for (std::size_t i{}; i < p.bindings.size(); ++i) {
    const auto &binding = p.bindings[i];
    for (std::size_t j{}; j < binding.chord.tokens.size(); ++j) {
      const auto extra = binding.token_extra_fields.find(binding.chord.tokens[j]);
      if (extra == binding.token_extra_fields.end()) continue;
      if (auto error = validate_extensions(extra->second, ProfileObject::token,
              "$.bindings[" + std::to_string(i) + "].chord[" + std::to_string(j) + "]", 5))
        return error;
    }
  }
  return std::nullopt;
}

} // namespace layerc::core::detail
