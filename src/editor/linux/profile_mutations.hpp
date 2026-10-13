#pragma once

#include "layerc/core/profile.hpp"
#include "layerc/core/types.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::editor {

// Toolkit-free edits applied to a candidate profile. The editor session
// validates the complete candidate before it replaces the document, so these
// helpers never need to leave a profile half-edited on a failed parse.

// Adds a cooldown with a fresh id and returns that id. Bar panels gain it
// through the shared panel normalization.
std::string add_cooldown(layerc::core::GameProfile &profile);
// Adds an unbound copy of a cooldown's timing under a new id; empty when the
// source is missing.
std::string duplicate_cooldown(layerc::core::GameProfile &profile, std::string_view id);
// Adds a card panel listing every cooldown; returns its id.
std::string add_panel(layerc::core::GameProfile &profile);
// Removes a panel; false when no panel has the id.
bool remove_panel(layerc::core::GameProfile &profile, std::string_view panel_id);
// Adds or removes one cooldown from a panel's ordered membership.
bool set_panel_member(layerc::core::GameProfile &profile, std::string_view panel_id,
                      std::string_view cooldown_id, bool member);

// Bindings: each binding is one chord for one cooldown. Unsupported tokens
// (controller, HID) are kept unless the user removes that binding.
void add_binding(layerc::core::GameProfile &profile, std::string_view cooldown_id,
                 layerc::core::InputChord chord);
bool remove_binding(layerc::core::GameProfile &profile, std::size_t index);

// Duplicate of the profile with a fresh id (one `taken` rejects) and name.
[[nodiscard]] layerc::core::GameProfile
duplicate_profile(const layerc::core::GameProfile &profile,
                  const std::function<bool(std::string_view)> &taken);

// Parsers return nullopt for anything but a complete, in-range value.
[[nodiscard]] std::optional<double> parse_anchor(std::string_view text);
[[nodiscard]] std::optional<std::uint32_t> parse_rgb(std::string_view text);
[[nodiscard]] std::optional<std::uint32_t> parse_count(std::string_view text,
                                                       std::uint32_t minimum,
                                                       std::uint32_t maximum);

// Display formatting, round-tripping with the parsers above.
[[nodiscard]] std::string format_decimal(double value);
[[nodiscard]] std::string format_seconds(layerc::core::Milliseconds duration);
[[nodiscard]] std::string format_rgb(std::uint32_t color);
[[nodiscard]] std::string token_label(const layerc::core::InputToken &token,
                                      const layerc::core::GameProfile &profile);
[[nodiscard]] std::string chord_label(const layerc::core::InputChord &chord,
                                      const layerc::core::GameProfile &profile);

} // namespace layerc::editor
