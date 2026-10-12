#pragma once

#include "automation_presets.hpp"
#include "layerc/core/automation.hpp"
#include "layerc/core/profile.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layerc::editor {

// Toolkit-free automation graph editing shared by the editors: labels, the
// add palette, node rows, JSON error locations and the edits themselves. An
// edit works on the document's optional automation module against its base
// profile and returns the refusal text; frontends validate the result
// (validate_automation_graph) and record history.

// First "<prefix>-<n>" id unused by items, counting up from items.size() + 1.
template <typename Items>
[[nodiscard]] std::string numbered_unique_id(const std::string &prefix,
                                             const Items &items) {
  for (std::size_t suffix = items.size() + 1U;; ++suffix) {
    auto id = prefix + "-" + std::to_string(suffix);
    if (std::ranges::none_of(items,
                             [&](const auto &item) { return item.id == id; }))
      return id;
  }
}

[[nodiscard]] std::string
automation_label(const layerc::core::AutomationDefinition &automation);
[[nodiscard]] std::string_view
event_label(layerc::core::AutomationEventKind kind) noexcept;
[[nodiscard]] std::string_view
action_label(layerc::core::AutomationActionKind kind) noexcept;

// Starter presets in menu order (index = AutomationPreset).
inline constexpr std::array<std::string_view, 4> automation_preset_labels{
    "Blank", "Input starts a cooldown", "Repeating session timer",
    "Ready overlay pulse"};
// The editor's views and simulator speeds, in menu order.
inline constexpr std::array<std::string_view, 3> automation_view_labels{
    "Basic", "Simulator", "Advanced JSON"};
inline constexpr std::array<std::string_view, 3> simulator_speed_labels{
    "0.25x", "1x", "4x"};

enum class AutomationPaletteTarget { rule, action, variable };

struct AutomationPaletteEntry {
  std::string_view label;
  AutomationPaletteNode node;
  AutomationPaletteTarget target;
  bool requires_cooldown;
  bool requires_panel;
};

// The add palette, in menu order.
inline constexpr std::array automation_palette{
    AutomationPaletteEntry{"Event: input pressed",
                           AutomationPaletteNode::input_rule,
                           AutomationPaletteTarget::rule, true, false},
    AutomationPaletteEntry{"Event: timer fired",
                           AutomationPaletteNode::timer_rule,
                           AutomationPaletteTarget::rule, true, false},
    AutomationPaletteEntry{"Action: trigger cooldown",
                           AutomationPaletteNode::trigger_action,
                           AutomationPaletteTarget::action, true, false},
    AutomationPaletteEntry{"Action: create timer",
                           AutomationPaletteNode::timer_action,
                           AutomationPaletteTarget::action, true, false},
    AutomationPaletteEntry{"Action: overlay visibility",
                           AutomationPaletteNode::overlay_action,
                           AutomationPaletteTarget::action, true, true},
    AutomationPaletteEntry{"Variable: Boolean",
                           AutomationPaletteNode::flag_variable,
                           AutomationPaletteTarget::variable, false, false},
};

[[nodiscard]] inline const AutomationPaletteEntry *
automation_palette_entry(const std::ptrdiff_t index) noexcept {
  return index < 0 ||
                 static_cast<std::size_t>(index) >= automation_palette.size()
             ? nullptr
             : &automation_palette[static_cast<std::size_t>(index)];
}

// The selected automation and, within it, a rule and one of its actions;
// -1 means none.
struct AutomationSelection {
  int automation{-1};
  int rule{-1};
  int action{-1};
  friend bool operator==(const AutomationSelection &,
                         const AutomationSelection &) = default;
};

// One row of the rule/action list; a rule row has action == -1.
struct AutomationNodeRow {
  int rule{-1};
  int action{-1};
  std::string label;
};
[[nodiscard]] std::vector<AutomationNodeRow>
automation_node_rows(const layerc::core::AutomationDefinition &automation);

// The selection clamped to what the module contains.
[[nodiscard]] AutomationSelection
clamp_selection(const layerc::core::AutomationProfile &graph,
                AutomationSelection selection);
[[nodiscard]] AutomationSelection
clamp_selection(const std::optional<layerc::core::AutomationProfile> &graph,
                AutomationSelection selection);

// Empty when valid, else the validator's description.
[[nodiscard]] std::string
validate_automation_graph(const layerc::core::AutomationProfile &graph,
                          const layerc::core::GameProfile &profile);
// The one-line state the editors show under the graph.
[[nodiscard]] std::string automation_validation_summary(
    const std::optional<layerc::core::AutomationProfile> &graph,
    const layerc::core::GameProfile &profile);

// The serialized module, or a blank document when there is none.
[[nodiscard]] std::string
automation_graph_json(const std::optional<layerc::core::AutomationProfile> &graph);

// Where a JSON parse error points: "Line L, column C: ..." with the UTF-8
// byte range of the offending character when the error has a $@<offset> path.
struct JsonErrorLocation {
  std::string message;
  std::optional<std::size_t> begin;
  std::optional<std::size_t> end;
  std::size_t line{};
  std::size_t column{};
};
[[nodiscard]] JsonErrorLocation
locate_json_error(std::string_view source,
                  const layerc::core::AutomationError &error);

// The reason a preset or palette entry cannot be added now, if any.
[[nodiscard]] std::optional<std::string>
preset_unavailable(const layerc::core::GameProfile &profile,
                   AutomationPreset preset);
[[nodiscard]] std::optional<std::string> palette_unavailable(
    const layerc::core::GameProfile &profile,
    const std::optional<layerc::core::AutomationProfile> &graph,
    AutomationSelection selection, const AutomationPaletteEntry &entry);

// Edits. Each returns the new selection or the refusal text; the module is
// unchanged when refused.
using AutomationEditResult = std::variant<AutomationSelection, std::string>;
[[nodiscard]] AutomationEditResult
add_automation_preset(const layerc::core::GameProfile &profile,
                      std::optional<layerc::core::AutomationProfile> &graph,
                      AutomationPreset preset);
[[nodiscard]] AutomationEditResult
add_palette_node(const layerc::core::GameProfile &profile,
                 std::optional<layerc::core::AutomationProfile> &graph,
                 AutomationSelection selection,
                 const AutomationPaletteEntry &entry);
// Removing the last automation (with no variables) removes the module.
[[nodiscard]] AutomationSelection
remove_automation(std::optional<layerc::core::AutomationProfile> &graph,
                  AutomationSelection selection);
// Removes the selected action (and its rule when it was the last action) or
// the selected rule; an automation left without rules is removed too.
[[nodiscard]] AutomationSelection
delete_automation_node(std::optional<layerc::core::AutomationProfile> &graph,
                       AutomationSelection selection);

// The property fields for the selected rule and action, as text.
struct AutomationNodeFields {
  std::string rule_id;
  int event_index{-1};
  std::string source_filter;
  std::string priority;
  int action_kind_index{-1};
  std::string target;
  std::string value;
  bool repeating{};
  // The value is a non-literal expression the fields cannot show; it is
  // kept unless the value text is edited.
  bool advanced_value{};
};
inline constexpr std::string_view advanced_expression_text =
    "<Advanced expression>";
[[nodiscard]] AutomationNodeFields
automation_node_fields(const layerc::core::AutomationProfile &graph,
                       AutomationSelection selection);
// Applies fields to the selected rule (and action). keep_advanced_value
// leaves a non-literal value untouched.
[[nodiscard]] std::optional<std::string>
apply_automation_node_fields(layerc::core::AutomationProfile &graph,
                             AutomationSelection selection,
                             const AutomationNodeFields &fields,
                             bool keep_advanced_value);

} // namespace layerc::editor
