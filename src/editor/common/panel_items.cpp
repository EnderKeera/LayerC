#include "panel_items.hpp"

#include "profile_edit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <utility>

namespace layerc::editor {
namespace {
using namespace core;
using Graph = std::optional<AutomationProfile>;
using Error = std::optional<ItemEditError>;

const CooldownDefinition *cooldown(const GameProfile &profile, const std::string_view id) {
  const auto found = std::ranges::find(profile.cooldowns, id, &CooldownDefinition::id);
  return found == profile.cooldowns.end() ? nullptr : &*found;
}
const std::string *variable_source(const OverlayItem &item) {
  if (const auto *counter = std::get_if<CounterItem>(&item.content)) return &counter->variable;
  if (const auto *indicator = std::get_if<IndicatorItem>(&item.content)) return &indicator->variable;
  return nullptr;
}
bool timer_declared(const AutomationProfile &graph, const std::string_view id) {
  for (const auto &automation : graph.automations)
    for (const auto &rule : automation.rules)
      for (const auto &action : rule.actions)
        if (action.kind == AutomationActionKind::create_timer && action.target == id) return true;
  return false;
}
Error source_error(const GameProfile &profile, const Graph &graph, const OverlayItem &item) {
  const auto *cd = std::get_if<CooldownItem>(&item.content);
  const auto *active = std::get_if<CooldownActiveTimerItem>(&item.content);
  if (cd || active) {
    const auto *source = cooldown(profile, cd ? cd->cooldown_id : active->cooldown_id);
    if (!source) return ItemEditError{"The selected cooldown no longer exists."};
    if (active && source->active_duration <= Milliseconds::zero())
      return ItemEditError{"Set Active duration on cooldown '" + source->name + "' first."};
    return {};
  }
  if (!graph || !validate_automation_profile(*graph, profile).message.empty())
    return ItemEditError{"Repair the automation module first."};
  if (const auto *timer = std::get_if<AutomationTimerItem>(&item.content)) {
    if (!timer_declared(*graph, timer->timer_id))
      return ItemEditError{"Select a declared automation timer."};
    return {};
  }
  const auto *id = variable_source(item);
  if (!id) return ItemEditError{"Unsupported item kinds cannot be edited."};
  const auto found = std::ranges::find(graph->variables, *id, &AutomationVariable::id);
  if (found == graph->variables.end()) return ItemEditError{"Select a declared automation variable."};
  const bool numeric = found->type == AutomationValueType::integer ||
                       found->type == AutomationValueType::number;
  if (const auto *counter = std::get_if<CounterItem>(&item.content)) {
    if (!numeric) return ItemEditError{"A counter needs an integer or number variable."};
    if (counter->display == CounterDisplay::pips &&
        (found->type != AutomationValueType::integer || !counter->max ||
         !std::isfinite(*counter->max) || std::floor(*counter->max) != *counter->max ||
         *counter->max < 1 || *counter->max > max_counter_pips))
      return ItemEditError{"Pips need an integer variable and an integer Max from 1 to 8."};
  } else if (!numeric && found->type != AutomationValueType::boolean) {
    return ItemEditError{"An indicator needs a boolean, integer or number variable."};
  }
  return {};
}
Error new_id(const Panel &panel, const ItemIdSource &ids, std::string &out) {
  if (!ids) return ItemEditError{"No item ID source is available."};
  for (unsigned attempt = 0; attempt < 64; ++attempt) {
    auto candidate = ids();
    if (!candidate.empty() && !candidate.starts_with("@legacy:") &&
        std::ranges::none_of(*panel.items, [&](const auto &item) { return item.id == candidate; })) {
      out = std::move(candidate);
      return {};
    }
  }
  return ItemEditError{"Cannot generate a unique item ID."};
}
Error promote(GameProfile &profile, Graph &graph, const std::size_t panel_index,
              const ItemIdSource &ids) {
  if (panel_index >= profile.panels.size()) return ItemEditError{"The selected panel no longer exists."};
  auto &panel = profile.panels[panel_index];
  if (panel.items) return {};
  panel.items.emplace();
  for (const auto &id : panel.cooldown_ids) {
    OverlayItem item;
    if (auto error = new_id(panel, ids, item.id)) return error;
    item.content = CooldownItem{id};
    panel.items->push_back(std::move(item));
  }
  if (graph)
    for (auto &automation : graph->automations)
      for (auto &rule : automation.rules)
        for (auto &action : rule.actions)
          if (action.kind == AutomationActionKind::set_overlay_override && action.target == panel.id)
            for (std::size_t index = 0; index < panel.items->size(); ++index)
              if (action.item_id == "@legacy:" + std::to_string(index)) {
                action.item_id = (*panel.items)[index].id;
                break;
              }
  panel.cooldown_ids.clear();
  return {};
}
Error validate(const GameProfile &profile, const Graph &before, const Graph &after) {
  if (const auto error = validate_profile(profile)) return ItemEditError{error->describe()};
  if (after && before != after) {
    const auto error = validate_automation_profile(*after, profile);
    if (!error.message.empty()) return ItemEditError{error.describe()};
  }
  return {};
}
template <typename Edit>
Error edit_items(GameProfile &profile, Graph &graph, const std::size_t panel,
                 const ItemIdSource &ids, Edit edit) {
  auto candidate = profile;
  auto automation = graph;
  if (auto error = promote(candidate, automation, panel, ids)) return error;
  if (auto error = edit(candidate, automation, candidate.panels[panel])) return error;
  if (auto error = validate(candidate, graph, automation)) return error;
  profile = std::move(candidate);
  graph = std::move(automation);
  return {};
}
Error removal_dependencies(const Panel &panel, const OverlayItem &item, const Graph &graph) {
  if (item.id.empty()) return {}; // Opaque rows are not referenceable item targets.
  std::string dependencies;
  if (graph)
    for (const auto &automation : graph->automations)
      for (const auto &rule : automation.rules)
        for (const auto &action : rule.actions)
          if (action.kind == AutomationActionKind::set_overlay_override &&
              action.target == panel.id && action.item_id == item.id)
            dependencies += "\n- Automation '" + automation.id + "', rule '" + rule.id + "'";
  if (!dependencies.empty())
    return ItemEditError{"Remove these item dependencies first:" + dependencies};
  return {};
}
} // namespace

std::string make_uuid_item_id() {
  std::random_device random;
  std::array<std::uint8_t, 16> bytes{};
  for (auto &byte : bytes) byte = static_cast<std::uint8_t>(random());
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
  constexpr std::string_view hex = "0123456789abcdef";
  std::string id;
  id.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) id += '-';
    id += hex[bytes[index] >> 4U];
    id += hex[bytes[index] & 0x0FU];
  }
  return id;
}
std::vector<std::string> panel_cooldown_members(const core::Panel &panel) {
  return panel.items ? core::overlay_cooldown_projection(*panel.items) : panel.cooldown_ids;
}
bool panel_has_cooldown(const core::Panel &panel, const std::string_view id) {
  const auto members = panel_cooldown_members(panel);
  return std::ranges::find(members, id) != members.end();
}
std::string item_display_name(const core::GameProfile &profile, const core::OverlayItem &item) {
  if (item.label) return *item.label;
  const auto *cd = std::get_if<core::CooldownItem>(&item.content);
  const auto *active = std::get_if<core::CooldownActiveTimerItem>(&item.content);
  if (cd || active) {
    const auto &id = cd ? cd->cooldown_id : active->cooldown_id;
    const auto *source = cooldown(profile, id);
    return source ? source->name : id;
  }
  if (const auto *timer = std::get_if<core::AutomationTimerItem>(&item.content)) return timer->timer_id;
  if (const auto *id = variable_source(item)) return *id;
  return item.id;
}
std::string item_kind_text(const core::OverlayItem &item) {
  switch (item.content.index()) {
  case 0: return "Cooldown";
  case 1: return "Timer (automation)";
  case 2: return "Timer (active)";
  case 3: return "Counter";
  case 4: return "Indicator";
  default: return "Unsupported kind";
  }
}
std::optional<ItemEditError> promote_panel_items(core::GameProfile &profile,
    Graph &graph, const std::size_t panel, const ItemIdSource &ids) {
  return edit_items(profile, graph, panel, ids, [](auto &, auto &, auto &) -> Error { return {}; });
}
std::optional<ItemEditError> add_panel_item(core::GameProfile &profile,
    Graph &graph, const std::size_t panel, core::OverlayItem item, const ItemIdSource &ids) {
  return edit_items(profile, graph, panel, ids, [&](auto &base, auto &automation, auto &target) -> Error {
    if (auto error = source_error(base, automation, item)) return error;
    if (auto error = new_id(target, ids, item.id)) return error;
    target.items->push_back(std::move(item));
    return {};
  });
}
std::optional<ItemEditError> update_panel_item(core::GameProfile &profile,
    Graph &graph, const std::size_t panel, const std::size_t index,
    core::OverlayItem item, const ItemIdSource &ids) {
  return edit_items(profile, graph, panel, ids, [&](auto &base, auto &automation, auto &target) -> Error {
    if (index >= target.items->size()) return ItemEditError{"The selected item no longer exists."};
    const auto &old = (*target.items)[index];
    if (!core::overlay_item_kind(old)) return ItemEditError{"Unsupported item kinds cannot be edited."};
    if (old.content != item.content)
      if (auto error = source_error(base, automation, item)) return error;
    item.id = old.id;
    (*target.items)[index] = std::move(item);
    return {};
  });
}
std::optional<ItemEditError> remove_panel_item(core::GameProfile &profile,
    Graph &graph, const std::size_t panel, const std::size_t index, const ItemIdSource &ids) {
  return edit_items(profile, graph, panel, ids, [&](auto &, auto &automation, auto &target) -> Error {
    if (index >= target.items->size()) return ItemEditError{"The selected item no longer exists."};
    if (auto error = removal_dependencies(target, (*target.items)[index], automation)) return error;
    if (target.items->size() == 1)
      return ItemEditError{"A panel needs at least one item; delete the panel instead."};
    target.items->erase(target.items->begin() + static_cast<std::ptrdiff_t>(index));
    return {};
  });
}
std::optional<ItemEditError> move_panel_item(core::GameProfile &profile,
    Graph &graph, const std::size_t panel, const std::size_t from,
    const std::size_t to, const ItemIdSource &ids) {
  return edit_items(profile, graph, panel, ids, [&](auto &, auto &, auto &target) -> Error {
    auto &items = *target.items;
    if (from >= items.size() || to >= items.size()) return ItemEditError{"Invalid item move."};
    auto moved = std::move(items[from]);
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(from));
    items.insert(items.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    return {};
  });
}
std::variant<TimerPresetResult, ItemEditError> add_automation_timer_preset(
    core::GameProfile &profile, Graph &graph, const std::size_t panel,
    const std::string_view cooldown_id, const core::Milliseconds interval,
    std::optional<std::string> label, const ItemIdSource &ids) {
  if (!cooldown(profile, cooldown_id)) return ItemEditError{"The selected cooldown no longer exists."};
  if (interval < core::min_automation_timer_interval || interval > core::max_cooldown_duration)
    return ItemEditError{"Timer interval must be from 50 ms to the maximum cooldown duration."};
  if (graph && !core::validate_automation_profile(*graph, profile).message.empty())
    return ItemEditError{"Repair the automation module first."};
  TimerPresetResult result;
  const auto error = edit_items(profile, graph, panel, ids,
      [&](auto &base, auto &automation, auto &target) -> Error {
    if (!automation) {
      automation.emplace();
      automation->profile_id = base.id;
    }
    result.timer_id = next_unique_id(std::string(cooldown_id) + "-active", [&](std::string_view id) {
      return timer_declared(*automation, id) ||
          std::ranges::any_of(automation->variables, [&](const auto &v) { return v.id == id; });
    });
    result.automation_id = next_unique_id(std::string(cooldown_id) + "-timer", [&](std::string_view id) {
      return std::ranges::any_of(automation->automations, [&](const auto &a) { return a.id == id; });
    });
    core::AutomationAction create;
    create.kind = core::AutomationActionKind::create_timer;
    create.target = result.timer_id;
    create.value = core::expr::literal(core::AutomationValue(core::DurationValue{interval}));
    core::AutomationRule rule;
    rule.id = "start-timer";
    rule.event = core::AutomationEventKind::cooldown_triggered;
    rule.source_filter = cooldown_id;
    rule.actions.push_back(std::move(create));
    automation->automations.push_back({.id = result.automation_id,
        .name = "Cooldown active timer", .enabled = true, .rules = {std::move(rule)}});
    core::OverlayItem item;
    if (auto id_error = new_id(target, ids, item.id)) return id_error;
    result.item_id = item.id;
    item.label = std::move(label);
    item.content = core::AutomationTimerItem{result.timer_id};
    target.items->push_back(std::move(item));
    return {};
  });
  return error ? std::variant<TimerPresetResult, ItemEditError>(*error)
               : std::variant<TimerPresetResult, ItemEditError>(std::move(result));
}
std::optional<std::string> item_variable_removal_error(
    const core::GameProfile &profile, const Graph &before, const Graph &after) {
  if (!before) return {};
  std::string dependencies;
  for (const auto &variable : before->variables) {
    if (after && std::ranges::any_of(after->variables,
        [&](const auto &candidate) { return candidate.id == variable.id; })) continue;
    for (const auto &panel : profile.panels)
      if (panel.items)
        for (const auto &item : *panel.items)
          if (const auto *id = variable_source(item); id && *id == variable.id)
            dependencies += "\n- Panel '" + panel.id + "', item '" + item.id +
                            "': variable '" + variable.id + "'";
  }
  if (dependencies.empty()) return {};
  return "Remove these variable dependencies first:" + dependencies;
}
} // namespace layerc::editor
