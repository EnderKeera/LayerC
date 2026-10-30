// Typed overlay item presentation: the one renderer-neutral contract every
// surface draws (PLAN §8.5).

#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/panel_presentation.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace layerc::test {
namespace {
using namespace layerc::core;
using namespace std::chrono_literals;

OverlayItem item(std::string id, OverlayItemContent content,
                 std::optional<std::string> label = std::nullopt) {
  OverlayItem result;
  result.id = std::move(id);
  result.label = std::move(label);
  result.content = std::move(content);
  return result;
}

Panel typed_panel(std::vector<OverlayItem> items) {
  Panel panel;
  panel.id = "main";
  panel.name = "Main";
  panel.style = PanelStyle::bar;
  panel.bar.hide_when_ready = true;
  panel.items = std::move(items);
  return panel;
}

const ItemPresentation *row(const PanelPresentation &shown, const std::string_view id) {
  for (const auto &entry : shown.items)
    if (entry.item_id == id) return &entry;
  return nullptr;
}

AutomationRuntimeSnapshot snapshot_with(const std::string &id, AutomationValue value) {
  AutomationRuntimeSnapshot snapshot;
  snapshot.variables.emplace(id, std::move(value));
  return snapshot;
}

PanelPresentation present_one(const OverlayItem &source,
                              const AutomationRuntimeSnapshot *automation,
                              const TimePoint now,
                              const PanelPresentationOverrides &overrides = {}) {
  const std::vector<CooldownDefinition> none;
  return present_panel(typed_panel({source}), none, {}, automation, overrides, now);
}
} // namespace

void test_panel_item_presentation_rows() {
  const auto start = TimePoint{} + 50s;
  CooldownDefinition dash;
  dash.id = "dash";
  dash.name = "Dash";
  dash.duration = 4s;
  dash.max_charges = 2;
  dash.recovery = RecoveryMode::sequential;
  dash.active_duration = 2s;
  CooldownDefinition blink;
  blink.id = "blink";
  blink.name = "Blink";
  blink.duration = 2s;
  const std::vector definitions{dash, blink};

  CooldownEngine engine(definitions);
  expect(engine.trigger("dash", start), "dash triggers");
  const auto states = engine.snapshot(start + 1s);

  auto panel = typed_panel({
      item("cd", CooldownItem{"dash"}),
      item("unknown", OpaqueOverlayItem{json::Value(json::Value::Object{})}),
      item("up", CooldownActiveTimerItem{"dash"}, std::string("Dash up")),
      item("quiet", CooldownItem{"blink"}, std::string()),
      item("hidden", CooldownItem{"blink"}),
  });
  panel.bar.fill_color = 0x112233;
  PanelPresentationOverrides overrides;
  overrides.items = {{.item_id = "hidden", .visible = false},
                     {.item_id = "quiet", .text = std::string("Q!"), .color = 0xABCDEFU}};
  auto shown = present_panel(panel, definitions, states, nullptr, overrides, start + 1s);
  expect(shown.items.size() == 3 && shown.items[0].item_id == "cd" &&
             shown.items[1].item_id == "up" && shown.items[2].item_id == "quiet",
         "rows keep authored order without opaque or hidden items");
  expect(shown.visible, "a recovering cooldown makes a hide-when-ready panel visible");
  const auto *cd = row(shown, "cd");
  expect(cd != nullptr && cd->label == "Dash  3.0s  [1]" && cd->kind == OverlayItemKind::cooldown &&
             !cd->ready && cd->active && cd->color == 0x112233,
         "typed cooldown rows match the legacy status text");
  const auto *up = row(shown, "up");
  expect(up != nullptr && up->kind == OverlayItemKind::timer && up->active &&
             up->remaining_time == 1s && up->remaining == 0.5 &&
             up->label == "Dash up  1.0s" && up->value_text == "1.0s" && up->cooldown_id == "dash",
         "cooldown-active timers count down their own window");
  const auto *quiet = row(shown, "quiet");
  expect(quiet != nullptr && quiet->label == "Q!" && quiet->color == 0xABCDEFU &&
             quiet->name.empty(),
         "item text and color overrides win over the source");
  expect(shown.next_wakeup == start + 2s, "the active window expiry is the next wakeup");

  overrides.text = std::string("Panel");
  shown = present_panel(panel, definitions, states, nullptr, overrides, start + 1s);
  expect(row(shown, "cd")->label == "Panel" && row(shown, "quiet")->label == "Q!",
         "item text beats panel text, which beats the computed label");

  auto empty_label = present_panel(typed_panel({item("q", CooldownItem{"blink"}, std::string())}),
                                   definitions, states, nullptr, {}, start + 1s);
  expect(empty_label.items.front().label == "READY" && !empty_label.visible,
         "an empty label shows the value alone; an idle bar panel hides");

  const auto expired = engine.snapshot(start + 2s);
  shown = present_panel(panel, definitions, expired, nullptr, {}, start + 2s);
  expect(!row(shown, "up")->active && row(shown, "up")->remaining == 0.0 &&
             row(shown, "up")->label == "Dash up",
         "an expired active window is stopped");

  auto disabled = definitions;
  disabled[0].active_duration = 0ms;
  shown = present_panel(panel, disabled, states, nullptr, {}, start + 1s);
  expect(row(shown, "up")->unavailable &&
             row(shown, "up")->unavailable_reason == ItemUnavailableReason::disabled_source &&
             row(shown, "up")->label == "Dash up  Unavailable",
         "a cooldown without an active duration is an unavailable source");
  shown = present_panel(panel, {}, {}, nullptr, {}, start);
  expect(row(shown, "cd")->unavailable &&
             row(shown, "cd")->unavailable_reason == ItemUnavailableReason::missing_source &&
             row(shown, "cd")->label == "dash  Unavailable" && shown.visible,
         "missing sources are unavailable rows that need attention");

  Panel legacy;
  legacy.id = "legacy";
  legacy.cooldown_ids = {"dash", "missing", "blink"};
  PanelPresentationOverrides legacy_overrides;
  legacy_overrides.items = {{.item_id = "@legacy:2", .color = 0x010203U}};
  shown = present_panel(legacy, definitions, states, nullptr, legacy_overrides, start + 1s);
  expect(shown.items.size() == 2 && shown.items[0].item_id == "@legacy:0" &&
             shown.items[1].item_id == "@legacy:2" && shown.items[1].color == 0x010203U &&
             shown.items[0].label == "Dash  3.0s  [1]" && shown.items[1].label == "Blink  READY",
         "legacy rows keep their cooldowns index as a runtime ID");

  expect(legacy_item_id(3) == "@legacy:3" && parse_legacy_item_index("@legacy:0") == 0U &&
             parse_legacy_item_index("@legacy:12") == 12U &&
             !parse_legacy_item_index("@legacy:01") && !parse_legacy_item_index("@legacy:+1") &&
             !parse_legacy_item_index("@legacy:") && !parse_legacy_item_index("legacy:1") &&
             !parse_legacy_item_index("@legacy:1x"),
         "legacy item IDs are canonical decimals");
  Panel two;
  two.cooldown_ids = {"dash", "blink"};
  expect(panel_item_targetable(two, "@legacy:1") && !panel_item_targetable(two, "@legacy:2") &&
             !panel_item_targetable(two, "@legacy:01") &&
             panel_item_targetable(panel, "cd") && !panel_item_targetable(panel, "unknown") &&
             !panel_item_targetable(panel, "@legacy:0") && !panel_item_targetable(panel, "nope"),
         "only known typed items and existing legacy rows are targetable");

  // Shared overlay item diagnostics: unsupported preserved kinds and
  // unavailable known sources, read from the same presentation authority.
  AutomationRuntimeSnapshot timers;
  timers.declared_timer_ids = {"declared-timer"};
  timers.variables.emplace("stacks", AutomationValue(std::string("many")));
  GameProfile profile;
  profile.id = "diag";
  profile.cooldowns = definitions;
  profile.panels.push_back(typed_panel({
      item("cd", CooldownItem{"dash"}),
      item("gone", CooldownItem{"vanished"}),
      item("future", OpaqueOverlayItem{json::Value(json::Value::Object{
          {"id", json::Value(std::string("future"))},
          {"kind", json::Value(std::string("hologram"))}})}),
      item("timer", AutomationTimerItem{"undeclared-timer"}),
      item("stacks", CounterItem{"stacks", std::nullopt, CounterDisplay::number}),
  }));
  std::string opaque_before;
  json::append_value(opaque_before, std::get<OpaqueOverlayItem>((*profile.panels.front().items)[2].content).value);
  const auto faults = overlay_item_diagnostics(profile, &timers, start + 1s);
  const auto has_fault = [&](const std::string_view text) {
    return std::ranges::any_of(
        faults, [&](const std::string &line) { return line.find(text) != std::string::npos; });
  };
  expect(has_fault("profile diag, panel main, item future: unsupported item kind") &&
             has_fault("profile diag, panel main, item gone: missing source") &&
             has_fault("profile diag, panel main, item stacks: source has incompatible type"),
         "helper diagnostics name profile, panel and item and distinguish kinds");
  expect(std::ranges::none_of(faults, [](const std::string &line) {
           return line.find(", item cd:") != std::string::npos;
         }) &&
             std::ranges::any_of(faults, [](const std::string &line) {
               return line.find(", item timer: missing source") != std::string::npos;
             }),
         "a valid row reports nothing while an undeclared timer is a warning");
  std::string opaque_after;
  json::append_value(opaque_after, std::get<OpaqueOverlayItem>((*profile.panels.front().items)[2].content).value);
  expect(row(present_panel(profile.panels.front(), definitions, states, &timers, {}, start + 1s),
             "future") == nullptr &&
             opaque_before == opaque_after,
         "the opaque row is absent from presentation and preserved byte-for-byte");
  expect(overlay_item_diagnostics(profile, nullptr, start + 1s).size() >= 3 &&
             std::ranges::any_of(overlay_item_diagnostics(profile, nullptr, start + 1s),
                                 [](const std::string &line) {
                                   return line.find(", item stacks: missing automation module") !=
                                          std::string::npos;
                                 }),
         "a null snapshot reports automation-bound items as missing automation");
  GameProfile legacy_only;
  legacy_only.id = "legacy-only";
  legacy_only.cooldowns = definitions;
  Panel legacy_panel;
  legacy_panel.id = "main";
  legacy_panel.cooldown_ids = {"dash"};
  legacy_only.panels.push_back(std::move(legacy_panel));
  expect(overlay_item_diagnostics(legacy_only, nullptr, start + 1s).empty(),
         "a legacy-only profile adds no overlay item warnings");
}

void test_panel_item_presentation_values() {
  const auto now = TimePoint{} + 20s;

  // Automation timers.
  AutomationRuntimeSnapshot timers;
  timers.declared_timer_ids = {"other", "rage"};
  timers.timers.push_back({.id = "rage", .owner_automation = "a", .due = now + 750ms,
                           .interval = 3s, .repeating = false});
  auto shown = present_one(item("t", AutomationTimerItem{"rage"}, std::string("Rage")), &timers, now);
  expect(shown.items.front().active && shown.items.front().remaining == 0.25 &&
             shown.items.front().label == "Rage  0.8s" && shown.next_wakeup == now + 750ms &&
             shown.visible,
         "automation timers count down their runtime interval");
  shown = present_one(item("t", AutomationTimerItem{"other"}), &timers, now);
  expect(!shown.items.front().active && !shown.items.front().unavailable &&
             shown.items.front().label == "other" && !shown.visible,
         "a declared timer absent from the runtime is stopped");
  shown = present_one(item("t", AutomationTimerItem{"removed"}), &timers, now);
  expect(shown.items.front().unavailable && shown.visible &&
             shown.items.front().unavailable_reason == ItemUnavailableReason::missing_source,
         "an undeclared timer is visibly unavailable, not normally stopped");
  timers.timers.clear();
  shown = present_one(item("t", AutomationTimerItem{"rage"}), &timers, now);
  expect(!shown.items.front().active && !shown.items.front().unavailable,
         "expiry or cancellation preserves a declared stopped timer source");
  shown = present_one(item("t", AutomationTimerItem{"rage"}), nullptr, now);
  expect(shown.items.front().unavailable &&
             shown.items.front().unavailable_reason == ItemUnavailableReason::missing_automation,
         "no automation module makes automation items unavailable");

  // Counters.
  const auto counter = [&](AutomationValue value, std::optional<double> max = std::nullopt,
                           CounterDisplay display = CounterDisplay::number) {
    const auto automation = snapshot_with("stacks", std::move(value));
    return present_one(item("c", CounterItem{"stacks", max, display}, std::string("Stacks")),
                       &automation, now)
        .items.front();
  };
  auto value = counter(AutomationValue(std::int64_t{3}), 5.0);
  expect(value.label == "Stacks  3/5" && value.value_text == "3/5" && value.has_progress &&
             value.remaining == 0.6 && value.active,
         "a counter with a maximum fills proportionally");
  value = counter(AutomationValue(std::numeric_limits<std::int64_t>::max()));
  expect(value.value_text == "9223372036854775807" && !value.has_progress,
         "integer counters format exactly without a maximum");
  value = counter(AutomationValue(std::int64_t{-4}), 5.0);
  expect(value.value_text == "-4/5" && value.remaining == 0.0 && value.active,
         "negative values keep their text and clamp the fill");
  value = counter(AutomationValue(std::int64_t{9}), 5.0);
  expect(value.value_text == "9/5" && value.remaining == 1.0, "overflowing values clamp the fill");
  value = counter(AutomationValue(2.5), 10.0);
  expect(value.value_text == "2.5/10" && value.remaining == 0.25, "number counters use core formatting");
  value = counter(AutomationValue(std::int64_t{0}));
  expect(!value.active && value.ready, "a zero counter is inactive");
  value = counter(AutomationValue(std::int64_t{3}), 8.0, CounterDisplay::pips);
  expect(value.pips == 8 && value.filled_pips == 3 && value.value_text == "3/8",
         "pips fill the integer value");
  value = counter(AutomationValue(std::int64_t{-7}), 4.0, CounterDisplay::pips);
  expect(value.pips == 4 && value.filled_pips == 0, "negative pip values fill nothing");
  value = counter(AutomationValue(std::numeric_limits<std::int64_t>::max()), 4.0,
                  CounterDisplay::pips);
  expect(value.filled_pips == 4, "huge pip values fill every pip");
  value = counter(AutomationValue(1.5), 4.0, CounterDisplay::pips);
  expect(value.unavailable && value.unavailable_reason == ItemUnavailableReason::wrong_type &&
             value.pips == 0,
         "pips require an integer value");
  value = counter(AutomationValue(true));
  expect(value.unavailable && value.unavailable_reason == ItemUnavailableReason::wrong_type,
         "a boolean is the wrong counter type");
  value = counter(AutomationValue(std::numeric_limits<double>::infinity()));
  expect(value.unavailable, "nonfinite counters are unavailable");
  const AutomationRuntimeSnapshot empty;
  shown = present_one(item("c", CounterItem{"missing", std::nullopt, CounterDisplay::number}), &empty, now);
  expect(shown.items.front().unavailable &&
             shown.items.front().unavailable_reason == ItemUnavailableReason::missing_source,
         "an undeclared variable is a missing source");

  // Indicators and flashing.
  const auto indicator = [&](AutomationValue truth, const double hz, const TimePoint at,
                             const TimePoint epoch = TimePoint{} + 20s) {
    const auto automation = snapshot_with("low", std::move(truth));
    PanelPresentationOverrides overrides;
    overrides.presentation_epoch = epoch;
    return present_one(item("i", IndicatorItem{"low", hz}, std::string("LOW")), &automation, at,
                       overrides);
  };
  expect(indicator(AutomationValue(true), 0.0, now).items.front().active &&
             !indicator(AutomationValue(false), 0.0, now).items.front().active &&
             indicator(AutomationValue(std::int64_t{-1}), 0.0, now).items.front().active &&
             !indicator(AutomationValue(0.0), 0.0, now).items.front().active &&
             !indicator(AutomationValue(std::numeric_limits<double>::quiet_NaN()), 0.0, now)
                  .items.front().active &&
             indicator(AutomationValue(std::string("x")), 0.0, now).items.front().unavailable,
         "indicator truth: boolean, nonzero finite numbers, other types unavailable");
  auto flash = indicator(AutomationValue(true), 0.0, now + 123ms);
  expect(flash.items.front().flash_on && !flash.next_wakeup && !flash.items.front().has_progress,
         "rate 0 is steady and needs no wakeup");
  flash = indicator(AutomationValue(true), 2.0, now);
  expect(flash.items.front().flash_on && flash.next_wakeup == now + 250ms,
         "phase zero is ON; the edge is half a period later");
  flash = indicator(AutomationValue(true), 2.0, now + 250ms);
  expect(!flash.items.front().flash_on && flash.next_wakeup == now + 500ms,
         "the second half period is OFF");
  flash = indicator(AutomationValue(true), 3.0, now + 166ms);
  expect(flash.items.front().flash_on && flash.next_wakeup == now + 167ms,
         "3 Hz rounds to 333 ms: phase 166 is still ON");
  flash = indicator(AutomationValue(true), 3.0, now + 167ms);
  expect(!flash.items.front().flash_on && flash.next_wakeup == now + 333ms,
         "phase 167 of 333 ms is OFF");
  flash = indicator(AutomationValue(true), 0.5, now + 1999ms);
  expect(!flash.items.front().flash_on && flash.next_wakeup == now + 2000ms,
         "0.5 Hz has a two second period");
  flash = indicator(AutomationValue(true), 4.0, now + 125ms);
  expect(!flash.items.front().flash_on && flash.next_wakeup == now + 250ms,
         "4 Hz flips every 125 ms");
  flash = indicator(AutomationValue(true), 2.0, now, now + 1s);
  expect(flash.items.front().flash_on, "an epoch in the future starts ON");
  flash = indicator(AutomationValue(false), 4.0, now + 125ms);
  expect(flash.items.front().flash_on && !flash.next_wakeup && !flash.visible,
         "inactive indicators never flash or wake");

  // Visibility.
  const auto counter_snapshot = snapshot_with("stacks", AutomationValue(std::int64_t{2}));
  PanelPresentationOverrides hide;
  hide.visible = false;
  hide.items = {{.item_id = "c", .visible = true}};
  shown = present_one(item("c", CounterItem{"stacks", std::nullopt, CounterDisplay::number}),
                      &counter_snapshot, now, hide);
  expect(!shown.visible && shown.items.size() == 1,
         "an explicit panel hide beats an item visible override");
  PanelPresentationOverrides edit;
  edit.edit_mode = true;
  shown = present_one(item("t", AutomationTimerItem{"other"}), &timers, now, edit);
  expect(shown.visible, "edit mode shows idle panels");

  // Declared timer inventory comes from the loaded profile, not authoring
  // preflight: defaults, a constructed runtime and reconfigure rebuild it.
  AutomationAction rage;
  rage.kind = AutomationActionKind::create_timer;
  rage.target = "rage";
  auto other_timer = rage;
  other_timer.target = "other";
  AutomationRule enabled;
  enabled.id = "enabled";
  enabled.actions = {rage, other_timer};
  AutomationRule disabled_rule;
  disabled_rule.id = "disabled";
  disabled_rule.actions = {rage};
  AutomationProfile graph;
  graph.profile_id = "declared";
  graph.automations = {
      {.id = "live", .name = "Live", .enabled = true, .rules = {enabled}},
      {.id = "off", .name = "Off", .enabled = false, .rules = {disabled_rule}}};
  const auto defaults = automation_default_snapshot(graph);
  expect(defaults.declared_timer_ids == std::vector<std::string>{"other", "rage"} &&
             defaults.timers.empty(),
         "declared defaults carry sorted unique timer IDs and no running timers");
  AutomationRuntime runtime(graph);
  expect(runtime.snapshot().declared_timer_ids == defaults.declared_timer_ids &&
             runtime.snapshot().timers.empty(),
         "a constructed runtime declares the same inventory without timer activity");
  graph.automations.front().rules.front().actions = {other_timer};
  graph.automations.back().rules.front().actions.clear();
  runtime.reconfigure(graph, {});
  expect(runtime.snapshot().declared_timer_ids == std::vector<std::string>{"other"} &&
             automation_default_snapshot(graph).declared_timer_ids ==
                 std::vector<std::string>{"other"},
         "reconfigure rebuilds the inventory from the new profile");
  shown = present_one(item("t", AutomationTimerItem{"rage"}), &runtime.snapshot(), now);
  expect(shown.items.front().unavailable &&
             shown.items.front().unavailable_reason == ItemUnavailableReason::missing_source,
         "a retained timer item whose declaration was removed is unavailable");
}

} // namespace layerc::test
