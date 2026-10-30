#include "panel_items.hpp"
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
  return [n = 0U]() mutable { return "row-" + std::to_string(++n); };
}
void sync_and_typed_removal() {
  auto profile = base();
  profile.panels.front().cooldown_ids.clear();
  profile.panels.front().items = std::vector<OverlayItem>{
      {.id = "counter", .content = CounterItem{.variable = "stacks"}}};
  sync_bar_panels(profile);
  expect(profile.panels.size() == 1 && profile.panels[0].items->size() == 2 &&
             profile.panels[0].items->front().id == "counter" &&
             panel_has_cooldown(profile.panels[0], "ability"),
         "sync retains typed non-cooldown rows and appends a missing cooldown with an ID");
  const auto before = profile;
  expect(!split_grouped_bars(profile, 1000) && profile == before,
         "split grouped bars does not rewrite typed panels");
  profile.cooldowns.front().active_duration = Milliseconds{1000};
  profile.panels.front().items->push_back(
      {.id = "active", .content = CooldownActiveTimerItem{"ability"}});
  auto other = profile.cooldowns.front();
  other.id = "other";
  profile.cooldowns.push_back(other);
  profile.bindings.push_back({.cooldown_id = "other",
      .chord = InputChord{{{InputDevice::keyboard, 0x13, 0}}}});
  AutomationProfile graph;
  graph.profile_id = profile.id;
  graph.variables.push_back({.id = "stacks", .type = AutomationValueType::integer,
                            .default_value = AutomationValue(std::int64_t{3})});
  ProfileDocument document;
  document.start_draft(profile, DraftOrigin::fresh, {}, graph);
  const auto error = document.remove_cooldown("ability");
  if (!expect(!error, error ? error->message : "typed cooldown removal succeeds")) return;
  expect(document.profile().panels.front().items->front().id == "counter" &&
             std::ranges::none_of(*document.profile().panels.front().items, [](const auto &item) {
               return std::holds_alternative<CooldownActiveTimerItem>(item.content);
             }), "cooldown deletion prunes both cooldown kinds but keeps unrelated typed rows");
}
void atomic_refusals() {
  const auto profile = base();
  layerc::platform::ProfileBundleLoad loaded;
  loaded.profile = profile;
  loaded.modules.get<AutomationModule>() = layerc::platform::AutomationSidecarLoad::invalid(
      {.path = "$", .message = "fixture invalid sidecar"}, {});
  ProfileDocument document;
  document.open({}, std::move(loaded));
  bool called = false;
  const auto blocked = document.edit_profile_and_automation([&](auto &, auto &) -> std::optional<std::string> {
    called = true;
    return {};
  }, DocumentChangeKind::immediate);
  expect(blocked.status == ModuleEditStatus::blocked && !called &&
             document.profile() == profile && document.has_unreadable_module() && !document.dirty(),
         "atomic item edits preserve unreadable automation and never invoke their mutation");
  document.start_draft(profile, DraftOrigin::fresh);
  const auto invalid = document.edit_profile_and_automation([](auto &p, auto &a) -> std::optional<std::string> {
    p.name = "Must not leak";
    a.emplace();
    a->profile_id = "different-profile";
    return {};
  }, DocumentChangeKind::immediate);
  expect(invalid.status == ModuleEditStatus::invalid && document.profile() == profile &&
             !document.automation() && !document.dirty() && !document.can_undo(),
         "invalid combined candidates change neither value nor history");
  auto mutable_profile = profile;
  std::optional<AutomationProfile> graph;
  const auto next = ids();
  expect(move_panel_item(mutable_profile, graph, 0, 0, 1, next).has_value() &&
             mutable_profile == profile && !graph,
         "out-of-range moves do not leak promotion");
}
void preset_collisions() {
  auto profile = base();
  AutomationProfile initial;
  initial.profile_id = profile.id;
  initial.variables.push_back({.id = "ability-active", .type = AutomationValueType::integer,
                              .default_value = AutomationValue(std::int64_t{1})});
  AutomationAction action;
  action.kind = AutomationActionKind::create_timer;
  action.target = "reserved";
  action.value = expr::literal(AutomationValue(DurationValue{Milliseconds{50}}));
  AutomationRule rule;
  rule.id = "keep-rule";
  rule.event = AutomationEventKind::session_started;
  rule.actions.push_back(action);
  initial.automations.push_back({.id = "ability-timer", .name = "Keep unchanged", .rules = {rule}});
  std::optional<AutomationProfile> graph = initial;
  const auto next = ids();
  const auto result = add_automation_timer_preset(profile, graph, 0, "ability", Milliseconds{50}, {}, next);
  const auto *added = std::get_if<TimerPresetResult>(&result);
  if (!expect(added != nullptr, "50 ms timer preset is accepted at the exact lower bound")) return;
  expect(added->timer_id != "ability-active" && added->automation_id != "ability-timer" &&
             graph->automations.front() == initial.automations.front() &&
             graph->variables == initial.variables,
         "preset avoids variable/automation IDs and preserves existing rules and defaults");
}
} // namespace

void editor_panel_item_additional_contracts() {
  sync_and_typed_removal();
  atomic_refusals();
  preset_collisions();
}
