#include "layerc/core/hash.hpp"
#include "layerc/core/hid.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/profile_modules.hpp"
#include "profile_extension_validation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

// The profile's semantic rules. Paths and messages match the JSON reader's
// so a document and an in-memory edit report the same error.
namespace layerc::core {
namespace {

constexpr std::size_t max_hid_bindings = 128;
constexpr std::string_view empty_text = "expected non-empty string";
constexpr std::string_view integer_range =
    "integer is outside the supported range or encoding";
constexpr std::string_view number_range = "number is outside the supported range";

using IdSet = std::set<std::string, std::less<>>;
using Result = std::optional<ProfileError>;

[[nodiscard]] std::string indexed(const std::string_view path,
                                  const std::size_t index) {
  return std::string(path) + "[" + std::to_string(index) + "]";
}

[[nodiscard]] ProfileError error(std::string path, const std::string_view message) {
  return {std::move(path), std::string(message)};
}

[[nodiscard]] bool within(const std::int64_t value, const std::int64_t minimum,
                          const std::int64_t maximum) noexcept {
  return value >= minimum && value <= maximum;
}

[[nodiscard]] bool within_number(const double value, const double minimum,
                          const double maximum) noexcept {
  return std::isfinite(value) && value >= minimum && value <= maximum;
}

[[nodiscard]] Result text(const std::string &path, const std::string_view key,
                          const std::string &value) {
  if (value.empty()) return error(path + "." + std::string(key), empty_text);
  return std::nullopt;
}

// Duration and charges shared by a cooldown and a shared-pool group.
[[nodiscard]] Result recharge(const std::string &path,
                              const Milliseconds duration,
                              const std::uint32_t max_charges) {
  if (!within(duration.count(), 1, max_cooldown_duration.count()))
    return error(path + ".durationMs", integer_range);
  if (!within(max_charges, min_cooldown_charges, max_cooldown_charges))
    return error(path + ".maxCharges", integer_range);
  return std::nullopt;
}

[[nodiscard]] Result validate_modules(const GameProfile &profile) {
  std::set<ProfileModuleKind> kinds;
  for (std::size_t index{}; index < profile.modules.size(); ++index) {
    const auto path = indexed("$.modules", index);
    const auto &module = profile.modules[index];
    const auto *descriptor = find_module(module.kind);
    if (descriptor == nullptr)
      return error(path + ".kind", "unsupported module kind");
    if (!within(module.schema_version, 1, descriptor->max_schema))
      return error(path + ".schemaVersion", integer_range);
    if (!kinds.insert(module.kind).second)
      return error(path + ".kind", "duplicate module kind");
  }
  return std::nullopt;
}

[[nodiscard]] Result validate_hid_binding(const HidControlBinding &binding,
                                          const std::string &path) {
  const auto device = path + ".device";
  if (!within(binding.device.stable_slot, 1, 32))
    return error(device + ".stableSlot", integer_range);
  if (binding.device.descriptor_hash.empty())
    return error(device + ".descriptorHash", empty_text);
  if (!is_lowercase_sha256(binding.device.descriptor_hash))
    return error(device + ".descriptorHash",
                 "expected a lower-case SHA-256 digest");
  const auto control = path + ".control";
  if (!within_number(binding.threshold, 0.0, 1.0))
    return error(control + ".threshold", number_range);
  if (!within_number(binding.hysteresis, 0.0, 0.49))
    return error(control + ".hysteresis", number_range);
  if (is_system_keyboard_or_mouse(binding.device) &&
      !binding.allow_system_class)
    return error(path + ".allowSystemClass",
                 "keyboard/mouse HID requires explicit opt-in");
  if ((binding.activation == HidActivationMode::axis_positive ||
       binding.activation == HidActivationMode::axis_negative) &&
      binding.hysteresis > std::min(binding.threshold, 1.0 - binding.threshold))
    return error(control + ".hysteresis",
                 "hysteresis crosses the axis threshold");
  return std::nullopt;
}

[[nodiscard]] Result validate_hid_bindings(const GameProfile &profile) {
  if (profile.hid_bindings.size() > max_hid_bindings)
    return error("$.hidBindings", "at most 128 HID bindings are allowed");
  IdSet ids;
  for (std::size_t index{}; index < profile.hid_bindings.size(); ++index) {
    const auto path = indexed("$.hidBindings", index);
    const auto &binding = profile.hid_bindings[index];
    if (binding.id.empty() || !ids.insert(binding.id).second)
      return error(path + ".id", "duplicate or empty HID binding id");
    if (auto failure = validate_hid_binding(binding, path)) return failure;
  }
  return std::nullopt;
}

[[nodiscard]] Result validate_cooldowns(const GameProfile &profile,
                                        IdSet &ids) {
  if (profile.cooldowns.empty())
    return error("$.cooldowns", "at least one cooldown is required");
  for (std::size_t index{}; index < profile.cooldowns.size(); ++index) {
    const auto path = indexed("$.cooldowns", index);
    const auto &cooldown = profile.cooldowns[index];
    if (auto failure = text(path, "id", cooldown.id)) return failure;
    if (auto failure = text(path, "name", cooldown.name)) return failure;
    if (!ids.insert(cooldown.id).second)
      return error(path + ".id", "duplicate cooldown id");
    if (auto failure =
            recharge(path, cooldown.duration, cooldown.max_charges))
      return failure;
  }
  return std::nullopt;
}

[[nodiscard]] Result validate_groups(const GameProfile &profile,
                                     const IdSet &cooldowns) {
  if (profile.cooldown_groups.size() > max_cooldown_groups)
    return error("$.cooldownGroups", "at most 64 groups are allowed");
  IdSet group_ids;
  IdSet shared_members;
  for (std::size_t index{}; index < profile.cooldown_groups.size(); ++index) {
    const auto path = indexed("$.cooldownGroups", index);
    const auto &group = profile.cooldown_groups[index];
    if (group.id.empty() || !group_ids.insert(group.id).second)
      return error(path + ".id", "duplicate or empty group id");
    if (auto failure = text(path, "name", group.name)) return failure;
    if (group.cooldown_ids.empty())
      return error(path + ".cooldowns", "group requires a member");
    const bool shared = group.mode == CooldownGroupMode::shared_pool;
    IdSet members;
    for (std::size_t item{}; item < group.cooldown_ids.size(); ++item) {
      const auto item_path = indexed(path + ".cooldowns", item);
      const auto &id = group.cooldown_ids[item];
      if (!cooldowns.contains(id))
        return error(item_path, "unknown cooldown id");
      if (!members.insert(id).second)
        return error(item_path, "duplicate group member");
      if (shared && !shared_members.insert(id).second)
        return error(item_path, "cooldown belongs to more than one shared pool");
    }
    if (shared)
      if (auto failure = recharge(path, group.duration, group.max_charges))
        return failure;
  }
  return std::nullopt;
}

[[nodiscard]] Result validate_panels(const GameProfile &profile,
                                     const IdSet &cooldowns) {
  if (profile.panels.empty())
    return error("$.panels", "at least one panel is required");
  IdSet ids;
  for (std::size_t index{}; index < profile.panels.size(); ++index) {
    const auto path = indexed("$.panels", index);
    const auto &panel = profile.panels[index];
    if (auto failure = text(path, "id", panel.id)) return failure;
    if (auto failure = text(path, "name", panel.name)) return failure;
    if (!ids.insert(panel.id).second)
      return error(path + ".id", "duplicate panel id");
    if (!within(panel.bar.width, min_bar_width, max_bar_width))
      return error(path + ".bar.width", integer_range);
    if (!within(panel.bar.height, min_bar_height, max_bar_height))
      return error(path + ".bar.height", integer_range);
    if (!within_number(panel.anchor_x, 0.0, 1.0))
      return error(path + ".anchorX", number_range);
    if (!within_number(panel.anchor_y, 0.0, 1.0))
      return error(path + ".anchorY", number_range);
    if (panel.cooldown_ids.empty())
      return error(path + ".cooldowns", "panel requires at least one cooldown id");
    for (std::size_t item{}; item < panel.cooldown_ids.size(); ++item)
      if (!cooldowns.contains(panel.cooldown_ids[item]))
        return error(indexed(path + ".cooldowns", item), "unknown cooldown id");
  }
  return std::nullopt;
}

[[nodiscard]] Result validate_token(const InputToken &token,
                                    const std::string &path,
                                    const std::size_t hid_bindings) {
  if (token.device == InputDevice::generic_hid) {
    if (!within(token.code, 1, static_cast<std::int64_t>(hid_bindings)))
      return error(path + ".bindingId", "unknown HID binding id");
  } else {
    const std::int64_t maximum = token.device == InputDevice::mouse ? 7
                                 : token.device == InputDevice::controller
                                     ? 18
                                     : 65'535;
    if (!within(token.code, 1, maximum))
      return error(path + ".code", integer_range);
    if (token.controller_index > 3)
      return error(path + ".controllerIndex", integer_range);
    if (token.device == InputDevice::controller &&
        !within(token.code, 1, 10) && !within(token.code, 13, 18))
      return error(path + ".code", "controller code must be 1-10 or 13-18");
  }
  if (token.device != InputDevice::controller && token.controller_index != 0)
    return error(path + ".controllerIndex",
                 "controllerIndex is valid only for controller tokens");
  return std::nullopt;
}

[[nodiscard]] Result validate_bindings(const GameProfile &profile,
                                       const IdSet &cooldowns) {
  if (profile.bindings.empty())
    return error("$.bindings", "at least one binding is required");
  for (std::size_t index{}; index < profile.bindings.size(); ++index) {
    const auto path = indexed("$.bindings", index);
    const auto &binding = profile.bindings[index];
    if (auto failure = text(path, "cooldownId", binding.cooldown_id))
      return failure;
    if (!cooldowns.contains(binding.cooldown_id))
      return error(path + ".cooldownId", "unknown cooldown id");
    if (binding.chord.tokens.empty())
      return error(path + ".chord", "chord requires at least one token");
    std::set<InputToken> tokens;
    for (std::size_t item{}; item < binding.chord.tokens.size(); ++item) {
      const auto token_path = indexed(path + ".chord", item);
      const auto &token = binding.chord.tokens[item];
      if (auto failure =
              validate_token(token, token_path, profile.hid_bindings.size()))
        return failure;
      if (!tokens.insert(token).second)
        return error(token_path, "duplicate token in chord");
    }
  }
  return std::nullopt;
}

} // namespace

std::optional<ProfileError> validate_profile(const GameProfile &profile) {
  if (auto failure = text("$", "id", profile.id)) return failure;
  if (auto failure = text("$", "name", profile.name)) return failure;
  if (!profile.game.global && profile.game.executable_path.empty() &&
      profile.game.executable_name.empty())
    return error("$.game", "executablePath or executableName is required");
  if (auto failure = validate_modules(profile)) return failure;
  if (auto failure = validate_hid_bindings(profile)) return failure;
  IdSet cooldowns;
  if (auto failure = validate_cooldowns(profile, cooldowns)) return failure;
  if (auto failure = validate_groups(profile, cooldowns)) return failure;
  if (auto failure = validate_panels(profile, cooldowns)) return failure;
  if (auto failure = validate_bindings(profile, cooldowns)) return failure;
  return detail::validate_profile_extensions(profile);
}

} // namespace layerc::core
