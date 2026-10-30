// Item-scoped setOverlayOverride: wire, validation, runtime layering and
// session admission (PLAN §8.6).

#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/automation_session.hpp"
#include "layerc/core/panel_presentation.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace layerc::test {
namespace {
using namespace layerc::core;
using namespace std::chrono_literals;

AutomationExpression literal(AutomationValue value) {
  AutomationExpression expression;
  expression.kind = ExpressionKind::literal;
  expression.literal = std::move(value);
  return expression;
}

OverlayItem cooldown_item(std::string id) {
  OverlayItem item;
  item.id = std::move(id);
  item.content = CooldownItem{"dash"};
  return item;
}

GameProfile item_base() {
  GameProfile profile;
  profile.id = "game";
  profile.name = "Game";
  profile.game.executable_name = "game.exe";
  CooldownDefinition dash;
  dash.id = "dash";
  dash.name = "Dash";
  dash.duration = 5s;
  profile.cooldowns.push_back(dash);
  Panel typed;
  typed.id = "main";
  typed.name = "Main";
  OverlayItem opaque;
  opaque.id = "op";
  opaque.content = OpaqueOverlayItem{json::Value(json::Value::Object{
      {"id", json::Value(std::string("op"))},
      {"kind", json::Value(std::string("future"))}})};
  typed.items = std::vector{cooldown_item("cd"), cooldown_item("cd2"), opaque};
  profile.panels.push_back(typed);
  Panel legacy;
  legacy.id = "legacy";
  legacy.name = "Legacy";
  legacy.cooldown_ids = {"dash"};
  profile.panels.push_back(legacy);
  return profile;
}

AutomationAction overlay(std::string name, std::string item, std::string property,
                         AutomationValue value, std::string panel = "main") {
  AutomationAction action;
  action.kind = AutomationActionKind::set_overlay_override;
  action.target = std::move(panel);
  action.name = std::move(name);
  action.property = std::move(property);
  action.item_id = std::move(item);
  action.value = literal(std::move(value));
  return action;
}

AutomationProfile automation_with(std::vector<AutomationRule> rules) {
  AutomationProfile profile;
  profile.profile_id = "game";
  profile.automations.push_back(
      {.id = "auto", .name = "Auto", .enabled = true, .rules = std::move(rules)});
  return profile;
}

AutomationRule rule_on(std::string id, std::string source,
                       std::vector<AutomationAction> actions,
                       const std::int32_t priority = 0) {
  AutomationRule rule;
  rule.id = std::move(id);
  rule.priority = priority;
  rule.event = AutomationEventKind::input_pressed;
  rule.source_filter = std::move(source);
  rule.actions = std::move(actions);
  return rule;
}

std::string error_of(const AutomationProfile &automation, const GameProfile &base) {
  const auto error = validate_automation_profile(automation, base);
  return error.message.empty() ? std::string{} : error.path + "|" + error.message;
}

bool ends_with(const std::string &text, const std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

AutomationEvent press(const std::string &source, const TimePoint at,
                      const std::uint64_t origin) {
  return {.kind = AutomationEventKind::input_pressed,
          .timestamp = at,
          .source_id = source,
          .origin_token = origin,
          .fields = {}};
}
} // namespace

void test_automation_item_override_wire_and_validation() {
  const auto base = item_base();
  const auto valid = automation_with({rule_on(
      "set", "keyboard:1",
      {overlay("warn", "cd", "color", AutomationValue(std::int64_t{0xFF0000})),
       overlay("warn", "", "text", AutomationValue(std::string("Hot"))),
       overlay("warn", "@legacy:0", "visible", AutomationValue(false), "legacy")})});
  expect(error_of(valid, base).empty(), "item and panel scoped overrides validate");
  const auto encoded = serialize_automation_profile_json(valid);
  expect(encoded.find("\"itemId\"") != std::string::npos,
         "item-scoped actions serialize itemId");
  const auto parsed = parse_automation_profile_json(encoded, base);
  const auto *round_trip = std::get_if<AutomationProfile>(&parsed);
  expect(round_trip != nullptr && *round_trip == valid &&
             serialize_automation_profile_json(*round_trip) == encoded,
         "itemId round-trips byte-identically");

  auto panel_only = automation_with({rule_on(
      "set", "keyboard:1", {overlay("warn", "", "text", AutomationValue(std::string("Hot")))})});
  expect(serialize_automation_profile_json(panel_only).find("itemId") == std::string::npos,
         "panel-scoped actions never write itemId");

  auto wrong_action = panel_only;
  AutomationAction trigger;
  trigger.kind = AutomationActionKind::trigger_cooldown;
  trigger.target = "dash";
  trigger.item_id = "cd";
  wrong_action.automations.front().rules.front().actions = {trigger};
  const auto rejected = parse_automation_profile_json(
      serialize_automation_profile_json(wrong_action), base);
  const auto *rejected_error = std::get_if<AutomationError>(&rejected);
  expect(rejected_error != nullptr && ends_with(rejected_error->path, ".itemId") &&
             rejected_error->message == "only setOverlayOverride accepts itemId",
         "itemId on another action is rejected at its path");
  expect(ends_with(validate_automation_profile(wrong_action, base).path, ".itemId"),
         "in-memory itemId on another action fails validation");

  const auto check = [&](AutomationAction action, const std::string_view path,
                         const std::string_view message) {
    const auto automation = automation_with({rule_on("bad", "keyboard:1", {std::move(action)})});
    const auto error = validate_automation_profile(automation, base);
    return expect(ends_with(error.path, path) && error.message == message,
                  std::string(message) + " at " + std::string(path));
  };
  check(overlay("warn", "", "color", AutomationValue(std::int64_t{1})), ".property",
        "overlay property requires itemId");
  check(overlay("warn", "cd", "showText", AutomationValue(true)), ".property",
        "overlay property does not apply to items");
  check(overlay("warn", "nope", "text", AutomationValue(std::string("x"))), ".itemId",
        "unknown or opaque item ID for the target panel");
  check(overlay("warn", "op", "text", AutomationValue(std::string("x"))), ".itemId",
        "unknown or opaque item ID for the target panel");
  check(overlay("warn", "@legacy:1", "text", AutomationValue(std::string("x")), "legacy"),
        ".itemId", "unknown or opaque item ID for the target panel");
  check(overlay("warn", "cd", "color", AutomationValue(std::string("red"))), ".value",
        "overlay value does not match the property type");
}

void test_automation_item_override_runtime() {
  const auto base = item_base();
  const auto start = TimePoint{} + 30s;
  auto expiring = overlay("warn", "cd", "color", AutomationValue(std::int64_t{0xFF0000}));
  expiring.secondary = literal(AutomationValue(DurationValue{500ms}));
  AutomationAction clear;
  clear.kind = AutomationActionKind::clear_overlay_override;
  clear.target = "warn";
  auto automation = automation_with(
      {rule_on("set", "keyboard:1",
               {overlay("warn", "", "text", AutomationValue(std::string("Hot"))),
                overlay("warn", "cd", "text", AutomationValue(std::string("Item"))),
                overlay("warn", "cd2", "text", AutomationValue(std::string("Item2"))),
                expiring}),
       rule_on("low", "keyboard:4",
               {overlay("low", "cd", "text", AutomationValue(std::string("Low")))}, -5),
       rule_on("high", "keyboard:4",
               {overlay("high", "cd", "text", AutomationValue(std::string("High")))}, 5),
       rule_on("clear", "keyboard:2", {clear}),
       rule_on("bad", "keyboard:3",
               {overlay("bad", "nope", "text", AutomationValue(std::string("x"))),
                overlay("bad", "", "text", AutomationValue(std::string("Never")))})});
  AutomationSession session(base);
  (void)session.start(automation, {}, start, nullptr, false);
  (void)session.process(press("keyboard:1", start, 1), nullptr);
  const auto &overrides = session.runtime()->snapshot().overlay_overrides;
  expect(overrides.size() == 4, "one named layer holds panel and item properties");
  auto resolved = resolve_panel_overrides(base.panels.front(), overrides);
  expect(resolved.text == std::string("Hot") && resolved.items.size() == 2 &&
             resolved.items[0].item_id == "cd" && resolved.items[0].text == std::string("Item") &&
             resolved.items[0].color == 0xFF0000U && resolved.items[1].item_id == "cd2" &&
             resolved.items[1].text == std::string("Item2") && !resolved.items[1].color,
         "the same name and property coexist across items and the panel");

  (void)session.poll(start + 600ms, nullptr);
  resolved = resolve_panel_overrides(base.panels.front(),
                                     session.runtime()->snapshot().overlay_overrides);
  expect(resolved.items.size() == 2 && !resolved.items[0].color &&
             resolved.items[0].text == std::string("Item"),
         "item overrides expire individually");

  (void)session.process(press("keyboard:4", start + 700ms, 2), nullptr);
  resolved = resolve_panel_overrides(base.panels.front(),
                                     session.runtime()->snapshot().overlay_overrides);
  expect(resolved.items[0].text == std::string("High"),
         "the highest-priority layer wins per item and property");

  (void)session.process(press("keyboard:2", start + 800ms, 3), nullptr);
  const auto &after_clear = session.runtime()->snapshot().overlay_overrides;
  expect(std::ranges::none_of(after_clear, [](const auto &item) { return item.name == "warn"; }),
         "clearing a named layer clears its panel and item properties together");

  const auto before = session.runtime()->snapshot().overlay_overrides;
  (void)session.process(press("keyboard:3", start + 900ms, 4), nullptr);
  expect(session.runtime()->snapshot().overlay_overrides == before,
         "a bad item reference rejects the whole winning set at admission");

  PanelPresentationOverrides shown_overrides;
  shown_overrides.items = resolve_panel_overrides(base.panels.front(),
                                                  session.runtime()->snapshot().overlay_overrides)
                              .items;
  const auto shown = present_panel(base.panels.front(), base.cooldowns,
                                   session.snapshot(start + 900ms), session.presentation_snapshot(),
                                   shown_overrides, start + 900ms);
  expect(shown.items.size() == 2 && shown.items[0].label == "High",
         "resolved item overrides feed presentation");
}

} // namespace layerc::test
