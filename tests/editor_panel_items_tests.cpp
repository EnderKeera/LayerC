#include "panel_items.hpp"
#include "document_commit.hpp"
#include "profile_document.hpp"
#include "profile_edit.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace layerc::core;
using namespace layerc::editor;
using layerc::test::expect;

GameProfile base() {
  auto profile = make_new_profile();
  profile.game.executable_name = "fixture.exe";
  return profile;
}
ItemIdSource ids() {
  return [next = 0U]() mutable { return "item-" + std::to_string(++next); };
}
AutomationProfile variables(const GameProfile &profile) {
  AutomationProfile graph;
  graph.profile_id = profile.id;
  graph.variables.push_back({.id = "stacks", .type = AutomationValueType::integer,
                            .default_value = AutomationValue(std::int64_t{3})});
  graph.variables.push_back({.id = "ratio", .type = AutomationValueType::number,
                            .default_value = AutomationValue(1.5)});
  graph.variables.push_back({.id = "flag", .type = AutomationValueType::boolean,
                            .default_value = AutomationValue(true)});
  graph.variables.push_back({.id = "word", .type = AutomationValueType::string,
                            .default_value = AutomationValue(std::string("text"))});
  return graph;
}
AutomationProfile override_graph(const GameProfile &profile, std::string item_id) {
  AutomationAction action;
  action.kind = AutomationActionKind::set_overlay_override;
  action.target = profile.panels.front().id;
  action.item_id = std::move(item_id);
  action.name = "row-label";
  action.property = "text";
  action.value = expr::literal(AutomationValue(std::string("Live")));
  AutomationRule rule;
  rule.id = "row-rule";
  rule.event = AutomationEventKind::session_started;
  rule.actions.push_back(std::move(action));
  AutomationProfile graph;
  graph.profile_id = profile.id;
  graph.automations.push_back({.id = "row-automation", .name = "Row override",
                              .enabled = false, .rules = {std::move(rule)}});
  return graph;
}
void promotion_cases() {
  auto profile = base();
  profile.panels.front().cooldown_ids.push_back("ability");
  std::optional<AutomationProfile> graph = override_graph(profile, "@legacy:1");
  const auto original = profile;
  const auto original_graph = graph;
  const auto next = ids();
  expect(!promote_panel_items(profile, graph, 0, next), "legacy promotion succeeds");
  const auto &items = *profile.panels.front().items;
  expect(items.size() == 2 && items[0].id != items[1].id &&
             std::get<CooldownItem>(items[0].content).cooldown_id == "ability" &&
             std::get<CooldownItem>(items[1].content).cooldown_id == "ability",
         "duplicate legacy cooldown rows retain order and get distinct IDs");
  expect(graph->automations[0].rules[0].actions[0].item_id == items[1].id,
         "disabled rule aliases are remapped to their exact legacy row");
  expect(profile.panels.front().cooldown_ids.empty() &&
             validate_automation_profile(*graph, profile).message.empty(),
         "promotion clears the legacy list and retains a valid graph");
  const auto promoted = profile;
  const auto promoted_graph = graph;
  expect(!promote_panel_items(profile, graph, 0, next) && profile == promoted && graph == promoted_graph,
         "repeated promotion does not regenerate IDs");
  auto failed = original;
  auto failed_graph = original_graph;
  const ItemIdSource duplicate = [] { return "same-id"; };
  expect(promote_panel_items(failed, failed_graph, 0, duplicate).has_value() &&
             failed == original && failed_graph == original_graph,
         "ID exhaustion rolls back profile and override remapping");
  const auto uuid = make_uuid_item_id();
  expect(uuid.size() == 36 && uuid[8] == '-' && uuid[13] == '-' && uuid[14] == '4' &&
             uuid[18] == '-' && uuid[23] == '-' &&
             std::string_view("89ab").find(uuid[19]) != std::string_view::npos,
         "new IDs use UUID version 4 and RFC variant bits");
}
void source_and_limit_cases() {
  auto profile = base();
  std::optional<AutomationProfile> graph = variables(profile);
  const auto next = ids();
  expect(!add_panel_item(profile, graph, 0,
      {.content = CounterItem{.variable = "stacks", .max = 8, .display = CounterDisplay::pips}}, next),
      "integer counters support bounded pips");
  expect(!add_panel_item(profile, graph, 0,
      {.content = CounterItem{.variable = "ratio", .max = 5}}, next),
      "number counters support numeric display");
  expect(!add_panel_item(profile, graph, 0,
      {.content = IndicatorItem{.variable = "flag", .flash_hz = 0.5}}, next),
      "boolean indicator accepts the lower flash bound");
  const auto before = profile;
  const auto before_graph = graph;
  const auto refused = [&](OverlayItem item, const std::string_view message) {
    expect(add_panel_item(profile, graph, 0, std::move(item), next).has_value() &&
               profile == before && graph == before_graph, message);
  };
  refused({.content = CounterItem{.variable = "flag"}}, "boolean counter source is rejected atomically");
  refused({.content = CounterItem{.variable = "ratio", .max = 3, .display = CounterDisplay::pips}},
          "number variables cannot use pips");
  refused({.content = CounterItem{.variable = "stacks", .max = 2.5, .display = CounterDisplay::pips}},
          "fractional pips maximum is refused");
  refused({.content = CounterItem{.variable = "missing"}}, "undeclared counter source is refused");
  refused({.content = IndicatorItem{.variable = "word"}}, "string indicator source is refused");
  refused({.content = IndicatorItem{.variable = "flag", .flash_hz = 4.1}}, "flash above 4 Hz is refused");
  refused({.content = AutomationTimerItem{"missing"}}, "undeclared timer source is refused");
  refused({.content = CooldownActiveTimerItem{"ability"}}, "zero active duration cannot back a new timer");
  profile.cooldowns.front().active_duration = Milliseconds{1000};
  expect(!add_panel_item(profile, graph, 0, {.content = CooldownActiveTimerItem{"ability"}}, next),
         "positive active duration can back a timer");
  while (profile.panels.front().items->size() < max_overlay_items_per_panel)
    if (add_panel_item(profile, graph, 0, {.content = CooldownItem{"ability"}}, next)) return;
  const auto full = profile;
  expect(add_panel_item(profile, graph, 0, {.content = CooldownItem{"ability"}}, next).has_value() &&
             profile == full, "item count beyond 32 is refused without partial insertion");
}
void edit_and_dependency_cases() {
  auto profile = base();
  std::optional<AutomationProfile> graph;
  const auto next = ids();
  expect(!add_panel_item(profile, graph, 0, {.label = "Second", .content = CooldownItem{"ability"}}, next),
         "second cooldown item can be added");
  const auto target_id = profile.panels.front().items->back().id;
  graph = override_graph(profile, target_id);
  const auto original = profile;
  expect(remove_panel_item(profile, graph, 0, 1, next).has_value() && profile == original,
         "disabled automation override prevents removing its item");
  expect(!move_panel_item(profile, graph, 0, 1, 0, next) &&
             profile.panels.front().items->front().id == target_id,
         "reordering preserves item identity and its override");
  auto changed = profile.panels.front().items->front();
  changed.id = "cannot-rename";
  changed.label = "Renamed";
  expect(!update_panel_item(profile, graph, 0, 0, changed, next) &&
             profile.panels.front().items->front().id == target_id,
         "item edits preserve stable IDs");
  graph.reset();
  expect(!remove_panel_item(profile, graph, 0, 1, next), "unreferenced row can be removed");
  expect(remove_panel_item(profile, graph, 0, 0, next).has_value(),
         "removing the last row requires deleting the panel instead");
  const auto json = json::parse(R"({"id":"future","kind":"future","nested":{"x":1}})");
  const auto *value = std::get_if<json::Value>(&json);
  if (!expect(value != nullptr, "opaque row fixture parses")) return;
  graph = override_graph(profile, ""); // A panel-wide override must not block opaque removal.
  profile.panels.front().items->push_back({.content = OpaqueOverlayItem{*value}});
  const auto opaque = profile.panels.front().items->back();
  expect(!move_panel_item(profile, graph, 0, 1, 0, next) &&
             profile.panels.front().items->front() == opaque,
         "unsupported rows can move without rewriting their opaque payload");
  expect(update_panel_item(profile, graph, 0, 0, opaque, next).has_value(),
         "unsupported rows cannot be edited");
  expect(!remove_panel_item(profile, graph, 0, 0, next), "unsupported rows can be explicitly removed");
}
void preset_and_history_cases() {
  auto profile = base();
  std::optional<AutomationProfile> graph;
  ProfileDocument document;
  document.start_draft(profile, DraftOrigin::fresh);
  const auto next = ids();
  const auto outcome = document.edit_profile_and_automation([&](auto &p, auto &a) -> std::optional<std::string> {
    auto result = add_automation_timer_preset(p, a, 0, "ability", Milliseconds{1000}, "Active", next);
    if (const auto *error = std::get_if<ItemEditError>(&result)) return error->message;
    return {};
  }, DocumentChangeKind::immediate);
  if (!expect(outcome.accepted(), outcome.message)) return;
  const auto &saved = *document.automation();
  const auto &rule = saved.automations.front().rules.front();
  expect(rule.event == AutomationEventKind::cooldown_triggered && rule.source_filter == "ability" &&
             rule.actions.size() == 1 && rule.actions.front().kind == AutomationActionKind::create_timer,
         "preset creates the exact accepted-cooldown trigger and timer action");
  expect(!document.profile().cooldowns.front().active_duration.count() &&
             document.profile().panels.front().items->size() == 2,
         "automation preset is independent of standalone active duration");
  const auto promoted = document.profile();
  const auto automation = document.automation();
  expect(document.undo() && document.profile() == profile && !document.automation(),
         "one undo removes both the preset graph and its item/promotion");
  expect(document.redo() && document.profile() == promoted && document.automation() == automation,
         "one redo restores both parts with the same IDs");
  profile = promoted;
  graph = automation;
  auto second = add_automation_timer_preset(profile, graph, 0, "ability", Milliseconds{1000}, {}, next);
  expect(std::holds_alternative<TimerPresetResult>(second) && graph->automations.size() == 2 &&
             graph->automations[0].id != graph->automations[1].id &&
             graph->automations[0].rules[0].actions[0].target != graph->automations[1].rules[0].actions[0].target,
         "repeated presets generate distinct graph and timer identities");
  const auto before = profile;
  const auto before_graph = graph;
  expect(std::holds_alternative<ItemEditError>(add_automation_timer_preset(
      profile, graph, 0, "ability", Milliseconds{49}, {}, next)) &&
      profile == before && graph == before_graph, "sub-50 ms preset is atomic and refused");
}
void variable_dependency_cases() {
  auto profile = base();
  std::optional<AutomationProfile> graph = variables(profile);
  const auto next = ids();
  expect(!add_panel_item(profile, graph, 0, {.content = CounterItem{.variable = "stacks"}}, next),
         "variable dependency fixture accepts a counter");
  ProfileDocument document;
  document.start_draft(profile, DraftOrigin::fresh, {}, graph);
  const auto outcome = document.edit_automation([](auto &candidate) -> std::optional<std::string> {
    std::erase_if(candidate->variables, [](const auto &variable) { return variable.id == "stacks"; });
    return {};
  }, DocumentChangeKind::immediate);
  expect(outcome.status == ModuleEditStatus::refused && outcome.message.find("stacks") != std::string::npos &&
             outcome.message.find(profile.panels.front().id) != std::string::npos && document.automation() == graph,
         "variable removal reports panel/item dependencies without changing the graph");
  auto unavailable = *profile.panels.front().items->rbegin();
  std::get<CounterItem>(unavailable.content).variable = "absent";
  profile.panels.front().items->back() = unavailable;
  document.start_draft(profile, DraftOrigin::fresh, {}, graph);
  expect(document.edit_automation([](auto &candidate) -> std::optional<std::string> {
    candidate->variables.back().default_value = AutomationValue(std::string("changed"));
    return {};
  }, DocumentChangeKind::immediate).accepted(),
         "unrelated edits retain an already unavailable item");
}
void legacy_removal_identity_cases() {
  auto profile = base();
  auto other = profile.cooldowns.front();
  other.id = "other";
  other.name = "Other";
  profile.cooldowns.push_back(other);
  profile.bindings.push_back({.cooldown_id = "other",
      .chord = InputChord{{{InputDevice::keyboard, 0x13, 0}}}});
  profile.panels.front().cooldown_ids.push_back("other");
  auto graph = override_graph(profile, "@legacy:1");
  ProfileDocument document;
  document.start_draft(profile, DraftOrigin::fresh, {}, graph);
  const auto error = document.remove_cooldown("ability");
  if (!expect(!error, error ? error->message : "legacy cooldown removal succeeds")) return;
  const auto &panel = document.profile().panels.front();
  expect(panel.items && panel.items->size() == 1 &&
             std::get<CooldownItem>(panel.items->front().content).cooldown_id == "other" &&
             document.automation()->automations[0].rules[0].actions[0].item_id == panel.items->front().id,
         "removing a preceding legacy row remaps surviving aliases before compaction");
  expect(document.undo() && document.profile() == profile && document.automation() == graph,
         "cooldown deletion and alias remapping share one undo step");
}
void upgrade_cases() {
  const auto profile = base();
  layerc::platform::ProfileBundleLoad loaded;
  loaded.profile = profile;
  loaded.base_schema_below_items = true;
  ProfileDocument document;
  document.open("fixture.json", loaded);
  expect(!document.bundle().upgrade_needed, "schema-4 profile is not upgraded on open");
  expect(document.edit_profile([](auto &candidate) {
    candidate.cooldowns.front().active_duration = Milliseconds{1000};
  }, DocumentChangeKind::immediate).accepted(), "standalone active duration is an ordinary edit");
  expect(document.bundle().upgrade_needed && document.bundle().items_upgrade_needed,
         "first active-duration save requires the explicit schema-5 warning");
  document.acknowledge_upgrade();
  expect(!document.bundle().upgrade_needed, "explicit acknowledgement permits the upgrade");
  document.open("fixture.json", loaded);
  bool prompted = false;
  const auto refused = commit_document(document, {}, {}, [&] {
    prompted = true;
    return false;
  }, layerc::platform::KeepModule{}, {}, [](ProfileBundle &candidate) {
    candidate.profile.cooldowns.front().active_duration = Milliseconds{1000};
  });
  expect(prompted && refused.result == CommitResult::upgrade_declined &&
             document.profile() == profile && !document.dirty(),
         "candidate-only schema upgrades prompt before any durable write or history change");
  document.open("fixture.json", loaded);
  const auto next = ids();
  expect(document.edit_profile_and_automation([&](auto &p, auto &a) -> std::optional<std::string> {
    if (auto error = add_panel_item(p, a, 0, {.content = CooldownItem{"ability"}}, next)) return error->message;
    return {};
  }, DocumentChangeKind::immediate).accepted() && document.bundle().items_upgrade_needed,
         "first structural item edit also requests schema-5 confirmation");
}
} // namespace

void editor_panel_item_additional_contracts();

void editor_panel_item_contracts() {
  editor_panel_item_additional_contracts();
  promotion_cases();
  source_and_limit_cases();
  edit_and_dependency_cases();
  preset_and_history_cases();
  variable_dependency_cases();
  legacy_removal_identity_cases();
  upgrade_cases();
}
