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

namespace extension_keys {
namespace k = profile_keys;
inline constexpr std::array root{
    k::schema_version, k::id, k::name, k::enabled, k::game, k::visual_runtime,
    k::modules, k::cooldowns, k::cooldown_groups, k::hid_bindings, k::panels, k::bindings};
inline constexpr std::array game{
    k::global, k::executable_path, k::executable_name, k::window_class, k::window_title};
inline constexpr std::array module{k::kind, k::schema_version};
inline constexpr std::array cooldown{
    k::id, k::name, k::duration_ms, k::max_charges, k::retrigger, k::recovery,
    k::stack_charges}; // Recognized legacy field, migrated to recovery.
inline constexpr std::array group{
    k::id, k::name, k::mode, k::cooldowns, k::duration_ms, k::max_charges,
    k::retrigger, k::recovery};
inline constexpr std::array hid{k::id, k::device, k::control, k::allow_system_class};
inline constexpr std::array device{
    k::vid, k::pid, k::version, k::usage_page, k::usage, k::descriptor_hash,
    k::stable_slot, k::serial};
inline constexpr std::array control{
    k::report_id, k::usage_page, k::usage, k::link_collection, k::activation,
    k::value, k::threshold, k::hysteresis};
inline constexpr std::array panel{
    k::id, k::name, k::layout, k::style, k::bar, k::anchor_x, k::anchor_y, k::cooldowns};
inline constexpr std::array bar{
    k::width, k::height, k::fill_color, k::background_color, k::border_color,
    k::text_color, k::show_text, k::hide_when_ready};
inline constexpr std::array binding{k::cooldown_id, k::chord};
inline constexpr std::array token{k::device, k::code, k::controller_index, k::binding_id};
} // namespace extension_keys

inline std::span<const std::string_view> known_profile_fields(const ProfileObject kind) {
  using namespace extension_keys;
  switch (kind) {
  case ProfileObject::root: return root;
  case ProfileObject::game: return game;
  case ProfileObject::module: return module;
  case ProfileObject::cooldown: return cooldown;
  case ProfileObject::group: return group;
  case ProfileObject::hid: return hid;
  case ProfileObject::device: return device;
  case ProfileObject::control: return control;
  case ProfileObject::panel: return panel;
  case ProfileObject::bar: return bar;
  case ProfileObject::binding: return binding;
  case ProfileObject::token: return token;
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
    fields.emplace(key, value);
  }
  return json::Value(std::move(fields));
}

} // namespace layerc::core::detail
