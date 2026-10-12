#include "automation_graph.hpp"

#include "layerc/core/json.hpp"

#include <charconv>
#include <iterator>
#include <system_error>
#include <utility>

namespace layerc::editor {
namespace {
namespace core = layerc::core;
using core::expr::literal;

core::AutomationDefinition *
selected_definition(std::optional<core::AutomationProfile> &graph,
                    const AutomationSelection selection) {
  return graph && selection.automation >= 0 &&
                 static_cast<std::size_t>(selection.automation) <
                     graph->automations.size()
             ? &graph->automations[static_cast<std::size_t>(selection.automation)]
             : nullptr;
}

std::string first_id(const auto &items) {
  return items.empty() ? std::string{} : items.front().id;
}
} // namespace

std::string automation_label(const core::AutomationDefinition &automation) {
  auto result = automation.name.empty() ? automation.id : automation.name;
  if (!automation.enabled)
    result += " (disabled)";
  return result;
}

std::string_view event_label(const core::AutomationEventKind kind) noexcept {
  using core::AutomationEventKind;
  switch (kind) {
  case AutomationEventKind::input_pressed:
    return "Input pressed";
  case AutomationEventKind::input_released:
    return "Input released";
  case AutomationEventKind::input_value_changed:
    return "Input value";
  case AutomationEventKind::visual_observation:
    return "Visual observation";
  case AutomationEventKind::cooldown_triggered:
    return "Cooldown triggered";
  case AutomationEventKind::cooldown_ready:
    return "Cooldown ready";
  case AutomationEventKind::group_triggered:
    return "Group triggered";
  case AutomationEventKind::group_ready:
    return "Group ready";
  case AutomationEventKind::session_started:
    return "Session started";
  case AutomationEventKind::session_stopping:
    return "Session stopping";
  case AutomationEventKind::variable_changed:
    return "Variable changed";
  case AutomationEventKind::timer_fired:
    return "Timer fired";
  }
  return "Session started";
}

std::string_view action_label(const core::AutomationActionKind kind) noexcept {
  const auto *spec = core::automation_action_spec(kind);
  return spec == nullptr ? std::string_view("Invalid action")
                         : spec->display_name;
}

std::vector<AutomationNodeRow>
automation_node_rows(const core::AutomationDefinition &automation) {
  std::vector<AutomationNodeRow> rows;
  for (std::size_t rule_index{}; rule_index < automation.rules.size();
       ++rule_index) {
    const auto &rule = automation.rules[rule_index];
    auto label =
        "Rule: " + rule.id + " \u2014 " + std::string(event_label(rule.event));
    if (!rule.source_filter.empty())
      label += " [" + rule.source_filter + "]";
    rows.push_back({static_cast<int>(rule_index), -1, std::move(label)});
    for (std::size_t action_index{}; action_index < rule.actions.size();
         ++action_index) {
      const auto &action = rule.actions[action_index];
      auto text = "    Action " + std::to_string(action_index + 1U) + ": " +
                  std::string(action_label(action.kind));
      if (!action.target.empty())
        text += " \u2014 " + action.target;
      rows.push_back({static_cast<int>(rule_index),
                      static_cast<int>(action_index), std::move(text)});
    }
  }
  if (automation.rules.empty())
    rows.push_back({-1, -1, "No rules. Choose a rule type and Add."});
  return rows;
}

AutomationSelection clamp_selection(const core::AutomationProfile &graph,
                                    AutomationSelection selection) {
  const auto count = graph.automations.size();
  if (count == 0U)
    return {};
  selection.automation = std::clamp(selection.automation, 0,
                                    static_cast<int>(count) - 1);
  const auto &automation =
      graph.automations[static_cast<std::size_t>(selection.automation)];
  if (selection.rule < 0 ||
      static_cast<std::size_t>(selection.rule) >= automation.rules.size())
    return {selection.automation, -1, -1};
  const auto &rule = automation.rules[static_cast<std::size_t>(selection.rule)];
  if (selection.action < 0 ||
      static_cast<std::size_t>(selection.action) >= rule.actions.size())
    selection.action = -1;
  return selection;
}

AutomationSelection
clamp_selection(const std::optional<core::AutomationProfile> &graph,
                const AutomationSelection selection) {
  return graph ? clamp_selection(*graph, selection) : AutomationSelection{};
}

std::string validate_automation_graph(const core::AutomationProfile &graph,
                                      const core::GameProfile &profile) {
  const auto error = core::validate_automation_profile(graph, profile);
  return error.message.empty() ? std::string{} : error.describe();
}

std::string automation_validation_summary(
    const std::optional<core::AutomationProfile> &graph,
    const core::GameProfile &profile) {
  if (!graph)
    return "No automations yet.";
  const auto error = validate_automation_graph(*graph, profile);
  return error.empty() ? "Valid \u2014 no errors" : error;
}

std::string
automation_graph_json(const std::optional<core::AutomationProfile> &graph) {
  return graph ? core::json::format(core::serialize_automation_profile_json(*graph))
               // Same CRLF line endings as json::format (Linux strips CR).
               : "{\r\n  \"schemaVersion\": 1,\r\n  \"profileId\": \"\",\r\n"
                 "  \"continuousInput\": {},\r\n  \"variables\": [],\r\n"
                 "  \"automations\": []\r\n}\r\n";
}

JsonErrorLocation locate_json_error(const std::string_view source,
                                    const core::AutomationError &error) {
  JsonErrorLocation location{.message = error.describe(),
                             .begin = std::nullopt,
                             .end = std::nullopt,
                             .line = 0,
                             .column = 0};
  if (!error.path.starts_with("$@") || error.path.size() == 2)
    return location;
  std::size_t offset{};
  for (const char digit : std::string_view(error.path).substr(2)) {
    if (digit < '0' || digit > '9')
      return location;
    offset = std::min(offset * 10U + static_cast<std::size_t>(digit - '0'),
                      source.size());
  }
  const auto prefix = source.substr(0, offset);
  const auto newline = prefix.rfind('\n');
  const auto line_prefix =
      newline == std::string_view::npos ? prefix : prefix.substr(newline + 1U);
  location.line = 1U + static_cast<std::size_t>(std::ranges::count(prefix, '\n'));
  // Columns count characters: UTF-8 continuation bytes do not start one.
  location.column =
      1U + static_cast<std::size_t>(std::ranges::count_if(
               line_prefix, [](const char byte) {
                 return (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U;
               }));
  std::size_t width{};
  if (offset < source.size()) {
    const auto lead = static_cast<unsigned char>(source[offset]);
    width = lead < 0x80U             ? 1U
            : (lead & 0xE0U) == 0xC0U ? 2U
            : (lead & 0xF0U) == 0xE0U ? 3U
                                      : 4U;
    width = std::min(width, source.size() - offset);
  }
  location.begin = offset;
  location.end = offset + std::max<std::size_t>(1U, width);
  location.message = "Line " + std::to_string(location.line) + ", column " +
                     std::to_string(location.column) + ": " + error.message +
                     " (" + error.path + ")";
  return location;
}

std::optional<std::string>
preset_unavailable(const core::GameProfile &profile,
                   const AutomationPreset preset) {
  if (preset != AutomationPreset::blank && profile.cooldowns.empty())
    return "Add a cooldown before creating this automation preset.";
  if (preset == AutomationPreset::ready_overlay_pulse && profile.panels.empty())
    return "The overlay preset requires at least one panel.";
  return std::nullopt;
}

std::optional<std::string>
palette_unavailable(const core::GameProfile &profile,
                    const std::optional<core::AutomationProfile> &graph,
                    const AutomationSelection selection,
                    const AutomationPaletteEntry &entry) {
  if (entry.requires_cooldown && profile.cooldowns.empty())
    return "This rule type needs at least one cooldown.";
  if (entry.target == AutomationPaletteTarget::action &&
      clamp_selection(graph, selection).rule < 0)
    return "Select a rule before adding an action.";
  if (entry.requires_panel && profile.panels.empty())
    return "Overlay actions need at least one overlay bar.";
  return std::nullopt;
}

AutomationEditResult
add_automation_preset(const core::GameProfile &profile,
                      std::optional<core::AutomationProfile> &graph,
                      const AutomationPreset preset) {
  if (auto refusal = preset_unavailable(profile, preset))
    return *std::move(refusal);
  if (!graph) {
    core::AutomationProfile created;
    created.profile_id = profile.id;
    graph = std::move(created);
  }
  graph->automations.push_back(make_automation_preset(
      preset, numbered_unique_id("automation", graph->automations),
      first_id(profile.cooldowns), first_id(profile.panels)));
  return AutomationSelection{
      static_cast<int>(graph->automations.size()) - 1,
      graph->automations.back().rules.empty() ? -1 : 0, -1};
}

AutomationEditResult
add_palette_node(const core::GameProfile &profile,
                 std::optional<core::AutomationProfile> &graph,
                 AutomationSelection selection,
                 const AutomationPaletteEntry &entry) {
  selection = clamp_selection(graph, selection);
  auto *automation = selected_definition(graph, selection);
  if (automation == nullptr)
    return std::string("Create an automation before adding to it.");
  if (auto refusal = palette_unavailable(profile, graph, selection, entry))
    return *std::move(refusal);
  const auto cooldown_id = first_id(profile.cooldowns);
  switch (entry.target) {
  case AutomationPaletteTarget::rule:
    automation->rules.push_back(make_palette_rule(
        entry.node, numbered_unique_id("rule", automation->rules), cooldown_id));
    return AutomationSelection{selection.automation,
                               static_cast<int>(automation->rules.size()) - 1,
                               -1};
  case AutomationPaletteTarget::action: {
    auto &rule = automation->rules[static_cast<std::size_t>(selection.rule)];
    rule.actions.push_back(make_palette_action(entry.node, rule.actions.size(),
                                               cooldown_id,
                                               first_id(profile.panels)));
    return AutomationSelection{selection.automation, selection.rule,
                               static_cast<int>(rule.actions.size()) - 1};
  }
  case AutomationPaletteTarget::variable:
    graph->variables.push_back(
        make_palette_variable(numbered_unique_id("flag", graph->variables)));
    return selection;
  }
  return selection;
}

AutomationSelection
remove_automation(std::optional<core::AutomationProfile> &graph,
                  AutomationSelection selection) {
  selection = clamp_selection(graph, selection);
  if (selection.automation < 0)
    return {};
  graph->automations.erase(graph->automations.begin() + selection.automation);
  if (graph->automations.empty() && graph->variables.empty())
    graph.reset();
  return clamp_selection(graph, {selection.automation, -1, -1});
}

AutomationSelection
delete_automation_node(std::optional<core::AutomationProfile> &graph,
                       AutomationSelection selection) {
  selection = clamp_selection(graph, selection);
  auto *automation = selected_definition(graph, selection);
  if (automation == nullptr || selection.rule < 0)
    return selection;
  auto &rules = automation->rules;
  auto &rule = rules[static_cast<std::size_t>(selection.rule)];
  if (selection.action >= 0) {
    rule.actions.erase(rule.actions.begin() + selection.action);
    if (rule.actions.empty())
      rules.erase(rules.begin() + selection.rule);
  } else {
    rules.erase(rules.begin() + selection.rule);
  }
  if (rules.empty())
    return remove_automation(graph, {selection.automation, -1, -1});
  return {selection.automation, -1, -1};
}

AutomationNodeFields
automation_node_fields(const core::AutomationProfile &graph,
                       AutomationSelection selection) {
  AutomationNodeFields fields;
  selection = clamp_selection(graph, selection);
  if (selection.rule < 0)
    return fields;
  const auto &rule = graph.automations[static_cast<std::size_t>(selection.automation)]
                         .rules[static_cast<std::size_t>(selection.rule)];
  fields.rule_id = rule.id;
  const auto &events = core::automation_event_kinds;
  if (const auto event = std::ranges::find(events, rule.event);
      event != events.end())
    fields.event_index = static_cast<int>(std::distance(events.begin(), event));
  fields.source_filter = rule.source_filter;
  fields.priority = std::to_string(rule.priority);
  if (selection.action < 0)
    return fields;
  const auto &action = rule.actions[static_cast<std::size_t>(selection.action)];
  const auto &specs = core::automation_action_specs;
  if (const auto kind = std::ranges::find(specs, action.kind,
                                          &core::AutomationActionSpec::kind);
      kind != specs.end())
    fields.action_kind_index = static_cast<int>(std::distance(specs.begin(), kind));
  fields.target = action.target;
  fields.repeating = action.repeating;
  if (action.value.has_value()) {
    fields.advanced_value = action.value->kind != core::ExpressionKind::literal;
    fields.value = fields.advanced_value
                       ? std::string(advanced_expression_text)
                       : core::automation_value_json(action.value->literal);
  }
  return fields;
}

std::optional<std::string>
apply_automation_node_fields(core::AutomationProfile &graph,
                             AutomationSelection selection,
                             const AutomationNodeFields &fields,
                             const bool keep_advanced_value) {
  selection = clamp_selection(graph, selection);
  if (selection.rule < 0)
    return "Select a rule or action first.";
  int priority{};
  const auto *first = fields.priority.data();
  const auto *last = first + fields.priority.size();
  if (const auto [end, error] = std::from_chars(first, last, priority);
      fields.priority.empty() || error != std::errc{} || end != last)
    return "Priority must be a whole number.";
  // Edit a copy so a refused value leaves the graph unchanged.
  auto candidate = graph;
  auto &rule = candidate.automations[static_cast<std::size_t>(selection.automation)]
                   .rules[static_cast<std::size_t>(selection.rule)];
  rule.id = fields.rule_id;
  if (fields.event_index >= 0 &&
      static_cast<std::size_t>(fields.event_index) <
          core::automation_event_kinds.size())
    rule.event =
        core::automation_event_kinds[static_cast<std::size_t>(fields.event_index)];
  rule.source_filter = fields.source_filter;
  rule.priority = priority;
  if (selection.action >= 0) {
    auto &action = rule.actions[static_cast<std::size_t>(selection.action)];
    if (fields.action_kind_index >= 0 &&
        static_cast<std::size_t>(fields.action_kind_index) <
            core::automation_action_specs.size())
      action.kind = core::automation_action_specs
                        [static_cast<std::size_t>(fields.action_kind_index)]
                            .kind;
    action.target = fields.target;
    action.repeating =
        action.kind == core::AutomationActionKind::create_timer &&
        fields.repeating;
    const auto expected = core::automation_action_value_type(action, candidate);
    if (expected.has_value() && !keep_advanced_value) {
      if (fields.value.empty())
        return "This action requires a typed JSON value.";
      const auto parsed = core::parse_automation_value_json(fields.value, *expected);
      const auto *value = std::get_if<core::AutomationValue>(&parsed);
      if (value == nullptr)
        return std::get<core::AutomationError>(parsed).describe();
      action.value = literal(*value);
    }
  }
  graph = std::move(candidate);
  return std::nullopt;
}

} // namespace layerc::editor
