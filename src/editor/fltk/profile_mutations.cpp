#include "profile_mutations.hpp"

#include "profile_document.hpp"
#include "profile_edit.hpp"
#include "layerc/core/hid.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <utility>

namespace layerc::editor {
namespace {

namespace core = layerc::core;

bool has_cooldown(const core::GameProfile &profile, const std::string_view id) {
  return std::ranges::any_of(profile.cooldowns,
                             [&](const auto &cooldown) { return cooldown.id == id; });
}

core::Panel *find_panel(core::GameProfile &profile, const std::string_view id) {
  const auto found = std::ranges::find_if(
      profile.panels, [&](const core::Panel &panel) { return panel.id == id; });
  return found == profile.panels.end() ? nullptr : &*found;
}

// Readable names for the Windows scan-code set used by profile bindings (the
// X11 backend translates XKB physical key names into the same codes).
struct KeyName {
  std::uint16_t code;
  const char *name;
};
constexpr std::array key_names{
    KeyName{0x01, "Esc"},       KeyName{0x0E, "Backspace"}, KeyName{0x0F, "Tab"},
    KeyName{0x1C, "Enter"},     KeyName{0x1D, "Left Ctrl"}, KeyName{0x2A, "Left Shift"},
    KeyName{0x36, "Right Shift"}, KeyName{0x38, "Left Alt"}, KeyName{0x39, "Space"},
    KeyName{0x3A, "Caps Lock"}, KeyName{0x0C, "-"},         KeyName{0x0D, "="},
    KeyName{0x1A, "["},         KeyName{0x1B, "]"},         KeyName{0x27, ";"},
    KeyName{0x28, "'"},         KeyName{0x29, "`"},         KeyName{0x2B, "\\"},
    KeyName{0x33, ","},         KeyName{0x34, "."},         KeyName{0x35, "/"},
    KeyName{0xE01D, "Right Ctrl"}, KeyName{0xE038, "Right Alt"},
    KeyName{0xE05B, "Left Super"}, KeyName{0xE05C, "Right Super"},
    KeyName{0xE048, "Up"},      KeyName{0xE050, "Down"},    KeyName{0xE04B, "Left"},
    KeyName{0xE04D, "Right"},   KeyName{0xE047, "Home"},    KeyName{0xE04F, "End"},
    KeyName{0xE049, "Page Up"}, KeyName{0xE051, "Page Down"},
    KeyName{0xE052, "Insert"},  KeyName{0xE053, "Delete"},  KeyName{0xE01C, "Keypad Enter"},
};

std::string scan_code_label(const std::uint16_t code) {
  for (const auto &entry : key_names)
    if (entry.code == code) return entry.name;
  // Letter and digit rows of the scan-code set 1 layout.
  constexpr std::string_view row_q = "QWERTYUIOP", row_a = "ASDFGHJKL", row_z = "ZXCVBNM";
  if (code >= 0x10 && code < 0x10 + row_q.size()) return std::string(1, row_q[code - 0x10]);
  if (code >= 0x1E && code < 0x1E + row_a.size()) return std::string(1, row_a[code - 0x1E]);
  if (code >= 0x2C && code < 0x2C + row_z.size()) return std::string(1, row_z[code - 0x2C]);
  if (code >= 0x02 && code <= 0x0B) return std::string(1, "1234567890"[code - 0x02]);
  if (code >= 0x3B && code <= 0x44) return "F" + std::to_string(code - 0x3B + 1);
  if (code == 0x57 || code == 0x58) return "F" + std::to_string(code - 0x57 + 11);
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "Key 0x%X", static_cast<unsigned>(code));
  return buffer;
}

} // namespace

std::string add_cooldown(core::GameProfile &profile) {
  auto id = next_unique_id("cooldown", [&](const std::string &candidate) {
    return has_cooldown(profile, candidate);
  });
  core::CooldownDefinition cooldown;
  cooldown.id = id;
  cooldown.name = "Cooldown " + std::to_string(profile.cooldowns.size() + 1);
  cooldown.duration = core::Milliseconds(10000);
  profile.cooldowns.push_back(std::move(cooldown));
  sync_bar_panels(profile, id);
  return id;
}

std::string duplicate_cooldown(core::GameProfile &profile, const std::string_view id) {
  const auto source = std::ranges::find(profile.cooldowns, id, &core::CooldownDefinition::id);
  if (source == profile.cooldowns.end()) return {};
  const auto original = *source; // add_cooldown may reallocate
  auto copy = add_cooldown(profile);
  auto &added = *std::ranges::find(profile.cooldowns, copy, &core::CooldownDefinition::id);
  added = original;
  added.id = copy;
  added.name = original.name + " copy";
  return copy;
}

std::string add_panel(core::GameProfile &profile) {
  core::Panel panel;
  panel.id = unique_panel_id("panel", profile.panels);
  panel.name = "Panel " + std::to_string(profile.panels.size() + 1);
  for (const auto &cooldown : profile.cooldowns) panel.cooldown_ids.push_back(cooldown.id);
  auto id = panel.id;
  profile.panels.push_back(std::move(panel));
  return id;
}

bool remove_panel(core::GameProfile &profile, const std::string_view panel_id) {
  return std::erase_if(profile.panels, [&](const core::Panel &panel) {
           return panel.id == panel_id;
         }) > 0;
}

bool set_panel_member(core::GameProfile &profile, const std::string_view panel_id,
                      const std::string_view cooldown_id, const bool member) {
  auto *panel = find_panel(profile, panel_id);
  if (!panel || !has_cooldown(profile, cooldown_id)) return false;
  auto &ids = panel->cooldown_ids;
  const auto found = std::ranges::find(ids, cooldown_id);
  if (member == (found != ids.end())) return false;
  if (member) {
    // Keep the profile's cooldown order inside the panel.
    const auto rank = [&](const std::string &id) {
      return std::ranges::find_if(profile.cooldowns,
                                  [&](const auto &c) { return c.id == id; }) -
             profile.cooldowns.begin();
    };
    const auto position = std::ranges::find_if(ids, [&](const std::string &id) {
      return rank(id) > rank(std::string(cooldown_id));
    });
    ids.insert(position, std::string(cooldown_id));
  } else {
    ids.erase(found);
  }
  return true;
}

void add_binding(core::GameProfile &profile, const std::string_view cooldown_id,
                 core::InputChord chord) {
  profile.bindings.push_back({std::string(cooldown_id), std::move(chord)});
}

bool remove_binding(core::GameProfile &profile, const std::size_t index) {
  if (index >= profile.bindings.size()) return false;
  const bool had_hid = std::ranges::any_of(profile.bindings[index].chord.tokens,
      [](const core::InputToken &token) { return token.device == core::InputDevice::generic_hid; });
  profile.bindings.erase(profile.bindings.begin() +
                         static_cast<std::ptrdiff_t>(index));
  if (had_hid) core::compact_hid_bindings(profile);
  return true;
}

core::GameProfile duplicate_profile(const core::GameProfile &profile,
                                    const std::function<bool(std::string_view)> &taken) {
  auto copy = profile;
  const auto base = (profile.id.empty() ? std::string("profile") : profile.id) + "-copy";
  copy.id = next_unique_id(base, [&](const std::string &id) { return taken(id); });
  copy.name = profile.name + " (copy)";
  return copy;
}

std::optional<double> parse_anchor(const std::string_view text) {
  if (text.empty()) return std::nullopt;
  double value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value) ||
      value < 0.0 || value > 1.0)
    return std::nullopt;
  return value;
}

std::optional<std::uint32_t> parse_rgb(std::string_view text) {
  if (text.starts_with('#')) text.remove_prefix(1);
  if (text.size() != 6) return std::nullopt;
  std::uint32_t value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + 6, value, 16);
  if (error != std::errc{} || end != text.data() + 6) return std::nullopt;
  return value;
}

std::optional<std::uint32_t> parse_count(const std::string_view text,
                                         const std::uint32_t minimum,
                                         const std::uint32_t maximum) {
  std::uint32_t value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size() ||
      value < minimum || value > maximum)
    return std::nullopt;
  return value;
}

std::string format_decimal(const double value) {
  char buffer[64];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value);
  return error == std::errc{} ? std::string(buffer, end) : std::string{};
}

std::string format_seconds(const core::Milliseconds duration) {
  return format_decimal(static_cast<double>(duration.count()) / 1000.0);
}

std::string format_rgb(const std::uint32_t color) {
  char buffer[8];
  std::snprintf(buffer, sizeof buffer, "#%06X", static_cast<unsigned>(color & 0xFFFFFFU));
  return buffer;
}

std::string token_label(const core::InputToken &token, const core::GameProfile &profile) {
  switch (token.device) {
  case core::InputDevice::keyboard: return scan_code_label(token.code);
  case core::InputDevice::mouse: return "Mouse button " + std::to_string(token.code);
  case core::InputDevice::generic_hid:
    if (token.code == 0 || token.code > profile.hid_bindings.size())
      return "HID binding unavailable";
    return "HID - " + profile.hid_bindings[token.code - 1U].id;
  case core::InputDevice::controller: break;
  }
  return "Controller " + std::to_string(token.controller_index + 1U) + ", input " +
         std::to_string(token.code);
}

std::string chord_label(const core::InputChord &chord, const core::GameProfile &profile) {
  std::string label;
  for (const auto &token : chord.tokens) {
    if (!label.empty()) label += " + ";
    label += token_label(token, profile);
  }
  return label.empty() ? "(empty)" : label;
}

} // namespace layerc::editor
