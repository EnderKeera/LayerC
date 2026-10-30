#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/profile_wire.hpp"
#include "../src/core/profile_extensions.hpp"
#include "../src/core/profile_keys.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace layerc::test {
namespace {
using namespace layerc::core;
using json::Value;
using Object = Value::Object;
using Array = Value::Array;

GameProfile base_profile(const std::uint32_t schema = legacy_profile_schema_version) {
  GameProfile profile;
  profile.schema_version = schema;
  profile.id = "fixture";
  profile.name = "Fixture";
  profile.game.executable_name = "fixture.exe";
  CooldownDefinition cooldown;
  cooldown.id = "ability";
  cooldown.name = "Ability";
  cooldown.duration = Milliseconds{5000};
  cooldown.max_charges = 1;
  cooldown.recovery = RecoveryMode::single;
  profile.cooldowns.push_back(std::move(cooldown));
  Panel panel;
  panel.id = "main";
  panel.name = "Main";
  panel.cooldown_ids = {"ability"};
  profile.panels.push_back(std::move(panel));
  InputBinding binding;
  binding.cooldown_id = "ability";
  binding.chord.tokens.push_back({InputDevice::keyboard, 69, 0});
  profile.bindings.push_back(std::move(binding));
  return profile;
}

Value root_value(const GameProfile &profile) {
  const auto parsed = json::parse(serialize_profile_json(profile));
  return std::get<Value>(parsed);
}
Object root_object(const GameProfile &profile) {
  return root_value(profile).as_object();
}
void put(Object &object, const std::string_view key, Value value) {
  object.insert_or_assign(std::string(key), std::move(value));
}
void put_panel(Object &root, const std::size_t index, Object panel) {
  auto panels = root.at("panels").as_array();
  panels.at(index) = Value(std::move(panel));
  put(root, "panels", Value(std::move(panels)));
}
Object panel_object(const Object &root, const std::size_t index = 0) {
  return root.at("panels").as_array().at(index).as_object();
}
void set_items(Object &root, Array items, const std::size_t panel = 0) {
  auto panel_fields = panel_object(root, panel);
  put(panel_fields, "items", Value(std::move(items)));
  put_panel(root, panel, std::move(panel_fields));
}
void set_cooldown_field(Object &root, const std::string_view key, Value value,
                        const std::size_t index = 0) {
  auto cooldowns = root.at("cooldowns").as_array();
  auto cooldown = cooldowns.at(index).as_object();
  put(cooldown, key, std::move(value));
  cooldowns[index] = Value(std::move(cooldown));
  put(root, "cooldowns", Value(std::move(cooldowns)));
}
void remove_root_field(Object &root, const std::string_view key) {
  root.erase(std::string(key));
}
Value item_value(Object object) { return Value(std::move(object)); }
Object cooldown_item(const std::string_view id = "item",
                     const std::string_view ref = "ability") {
  return {{"id", Value(std::string(id))}, {"kind", Value(std::string("cooldown"))},
          {"cooldownId", Value(std::string(ref))}};
}
Object counter_item(const std::string_view id = "counter",
                    const std::string_view variable = "stacks") {
  return {{"id", Value(std::string(id))}, {"kind", Value(std::string("counter"))},
          {"variable", Value(std::string(variable))}};
}
Object indicator_item(const std::string_view id = "indicator",
                      const std::string_view variable = "warning") {
  return {{"id", Value(std::string(id))}, {"kind", Value(std::string("indicator"))},
          {"variable", Value(std::string(variable))}};
}
Object timer_item(const std::string_view id, const std::string_view source,
                  const std::string_view ref_key, const std::string_view ref) {
  return {{"id", Value(std::string(id))}, {"kind", Value(std::string("timer"))},
          {"source", Value(std::string(source))},
          {std::string(ref_key), Value(std::string(ref))}};
}
ProfileResult parse_root(Object root) { return parse_profile_value(Value(std::move(root))); }
bool expect_path(const ProfileResult &result, const std::string_view path,
                 const std::string_view message) {
  const auto *error = std::get_if<ProfileError>(&result);
  return expect(error != nullptr && error->path == path, message);
}
bool expect_valid(const ProfileResult &result, const std::string_view message) {
  return expect(std::holds_alternative<GameProfile>(result), message);
}
std::vector<OverlayItem> counter_items(const std::size_t count) {
  std::vector<OverlayItem> items;
  items.reserve(count);
  for (std::size_t i{}; i < count; ++i) {
    OverlayItem item;
    item.id = "counter-" + std::to_string(i);
    item.content = CounterItem{.variable = "stacks"};
    items.push_back(std::move(item));
  }
  return items;
}
std::optional<std::string> validation_path(const GameProfile &profile) {
  if (const auto error = validate_profile(profile)) return error->path;
  return std::nullopt;
}
bool serializer_throws(const GameProfile &profile) {
  try {
    (void)serialize_profile_json(profile);
  } catch (const std::invalid_argument &) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

} // namespace

void test_profile_items_round_trip() {
  auto root = root_object(base_profile(profile_schema_version));
  set_cooldown_field(root, "activeDurationMs", Value(std::int64_t{6000}));
  auto opaque_parse = json::parse(
      R"json({"kind":"futureGauge","id":false,"cooldownId":{"guess":1},"flashHz":"not-a-number","raw":{"negativeZero":-0,"wide":18446744073709551615}})json");
  const auto opaque_value = std::get<Value>(opaque_parse);
  auto extension_parse = json::parse(R"json({"future":{"nested":[1,"kept"]}})json");
  const auto extension = std::get<Value>(extension_parse).as_object();
  Object first = cooldown_item("dash-cooldown");
  put(first, "futureTag", Value(extension));
  Object second = cooldown_item("dash-cooldown-copy");
  put(second, "label", Value(std::string{}));
  Object active = timer_item("active-window", "cooldownActive", "cooldownId", "ability");
  put(active, "label", Value(std::string("Dash active")));
  Object automation = timer_item("buff-timer", "automation", "timerId", "buff");
  Object counter = counter_item("stacks", "stackCount");
  put(counter, "max", Value(std::int64_t{5}));
  put(counter, "display", Value(std::string("pips")));
  Object indicator = indicator_item("warning", "lowHealth");
  put(indicator, "label", Value(std::string{}));
  put(indicator, "flashHz", Value(std::int64_t{0}));
  set_items(root, Array{item_value(std::move(first)), item_value(std::move(second)),
                        item_value(std::move(active)), item_value(std::move(automation)),
                        item_value(std::move(counter)), item_value(std::move(indicator)),
                        opaque_value});

  GameProfile expected = base_profile(profile_schema_version);
  expected.cooldowns.front().active_duration = Milliseconds{6000};
  auto make_item = [](std::string id, OverlayItemContent content,
                      std::optional<std::string> label = std::nullopt,
                      Object extra = {}) {
    OverlayItem item;
    item.id = std::move(id);
    item.label = std::move(label);
    item.content = std::move(content);
    item.extra_fields = std::move(extra);
    return item;
  };
  expected.panels.front().items = std::vector<OverlayItem>{
      make_item("dash-cooldown", CooldownItem{"ability"}, std::nullopt,
                Object{{"futureTag", Value(extension)}}),
      make_item("dash-cooldown-copy", CooldownItem{"ability"}, std::string{}),
      make_item("active-window", CooldownActiveTimerItem{"ability"},
                std::string("Dash active")),
      make_item("buff-timer", AutomationTimerItem{"buff"}),
      make_item("stacks", CounterItem{"stackCount", 5.0, CounterDisplay::pips}),
      make_item("warning", IndicatorItem{"lowHealth", 0.0}, std::string{}),
      make_item({}, OpaqueOverlayItem{opaque_value}),
  };
  expected.panels.front().cooldown_ids = {"ability", "ability"};

  const auto parsed = parse_root(std::move(root));
  const auto *profile = std::get_if<GameProfile>(&parsed);
  if (!expect(profile != nullptr && *profile == expected,
              "all known alternatives and the opaque item parse into the value model"))
    return;
  expect(profile->panels.front().items->size() == 7 &&
             std::holds_alternative<OpaqueOverlayItem>(
                 profile->panels.front().items->back().content),
         "unknown item stays in its original list position");
  expect(profile->panels.front().items->at(1).label == std::optional<std::string>{""} &&
             !profile->panels.front().items->at(0).label.has_value(),
         "an explicit empty label remains distinct from an absent label");
  expect(overlay_cooldown_projection(*profile->panels.front().items) ==
             std::vector<std::string>{"ability", "ability"},
         "projection retains duplicate cooldown entries and excludes timer sources");
  expect(overlay_item_kind(profile->panels.front().items->at(2)) ==
             OverlayItemKind::timer &&
             !overlay_item_kind(profile->panels.front().items->back()).has_value(),
         "kind helper maps both timer payloads and leaves opaque items unclassified");

  const auto encoded = serialize_profile_json(*profile);
  const auto reparsed = parse_profile_json(encoded);
  const auto *restored = std::get_if<GameProfile>(&reparsed);
  expect(restored && *restored == *profile &&
             serialize_profile_json(*restored) == encoded,
         "typed schema 5 data round-trips to byte-stable JSON");
  expect(encoded.find("\"negativeZero\": -0") != std::string::npos &&
             encoded.find("18446744073709551615") != std::string::npos &&
             encoded.find("activeUntil") == std::string::npos,
         "opaque numeric spellings survive and runtime activity is not serialized");
  if (restored && restored->panels.front().items) {
    const auto *opaque = std::get_if<OpaqueOverlayItem>(
        &restored->panels.front().items->back().content);
    expect(opaque && opaque->value == opaque_value,
           "opaque raw JSON value including guessed fields is retained unchanged");
  }
}

void test_profile_items_validation() {
  constexpr std::string_view items_path = "$.panels[0].items";
  constexpr std::string_view item_path = "$.panels[0].items[0]";
  auto root_with = [](Object item) {
    auto root = root_object(base_profile(profile_schema_version));
    set_items(root, Array{item_value(std::move(item))});
    return root;
  };
  expect_path(parse_root(root_with({{"id", Value(std::string("missing-kind"))}})),
              "$.panels[0].items[0].kind", "item kind is required");
  expect_path(parse_root(root_with({{"kind", Value(true)}})),
              "$.panels[0].items[0].kind", "item kind must be a string");
  auto wrong_source = timer_item("timer", "invalid", "timerId", "missing");
  expect_path(parse_root(root_with(std::move(wrong_source))),
              "$.panels[0].items[0].source", "timer source uses the declared enum");

  struct WrongField {
    Object item;
    std::string_view field;
  };
  const std::array wrong_fields{
      WrongField{cooldown_item("cd"), "timerId"},
      WrongField{cooldown_item("cd-source"), "source"},
      WrongField{cooldown_item("cd-variable"), "variable"},
      WrongField{timer_item("auto", "automation", "timerId", "t"), "cooldownId"},
      WrongField{timer_item("active", "cooldownActive", "cooldownId", "ability"), "timerId"},
      WrongField{timer_item("auto-variable", "automation", "timerId", "t"), "variable"},
      WrongField{counter_item("counter"), "source"},
      WrongField{counter_item("counter-timer"), "timerId"},
      WrongField{counter_item("counter-cooldown"), "cooldownId"},
      WrongField{indicator_item("indicator"), "max"},
      WrongField{indicator_item("indicator-display"), "display"},
      WrongField{indicator_item("indicator-source"), "source"},
      WrongField{indicator_item("indicator-timer"), "timerId"},
      WrongField{indicator_item("indicator-cooldown"), "cooldownId"},
  };
  for (const auto &test : wrong_fields) {
    auto item = test.item;
    put(item, test.field, Value(std::string("wrong-kind")));
    expect_path(parse_root(root_with(std::move(item))),
                std::string(item_path) + "." + std::string(test.field),
                "recognized fields for another payload are rejected at that field");
  }

  auto future = counter_item("counter", "missingVariable");
  put(future, "futurePayload", Value(Array{Value(std::int64_t{1}),
                                            Value(std::string("kept"))}));
  const auto future_result = parse_root(root_with(std::move(future)));
  const auto *future_profile = std::get_if<GameProfile>(&future_result);
  expect(future_profile && future_profile->panels.front().items->front().extra_fields
                               .contains("futurePayload"),
         "unrecognized known-item fields remain opaque extensions");
  expect_valid(parse_root(root_with(timer_item("unresolved", "automation",
                                                "timerId", "not-yet-created"))),
               "automation timer references are deferred to cross-document validation");

  auto duplicate_root = root_object(base_profile(profile_schema_version));
  set_items(duplicate_root, Array{item_value(cooldown_item("same")),
                                  item_value(cooldown_item("same"))});
  expect_path(parse_root(std::move(duplicate_root)),
              "$.panels[0].items[1].id", "duplicate known ids are panel-local errors");
  expect_path(parse_root(root_with(cooldown_item("@legacy:0"))),
              "$.panels[0].items[0].id", "explicit ids cannot use the legacy prefix");
  expect_path(parse_root(root_with(cooldown_item(""))),
              "$.panels[0].items[0].id", "known item IDs cannot be empty");
  expect_path(parse_root(root_with(cooldown_item("empty", ""))),
              "$.panels[0].items[0].cooldownId", "known cooldown references are nonempty");
  expect_path(parse_root(root_with(counter_item("empty-variable", ""))),
              "$.panels[0].items[0].variable", "counter variables are nonempty");
  expect_path(parse_root(root_with(timer_item("empty-timer", "automation",
                                               "timerId", ""))),
              "$.panels[0].items[0].timerId", "timer references are nonempty");

  auto multiple = base_profile(profile_schema_version);
  OverlayItem same_id_a;
  same_id_a.id = "panel-local";
  same_id_a.content = CooldownItem{"ability"};
  OverlayItem same_id_b = same_id_a;
  multiple.panels.front().items = std::vector<OverlayItem>{same_id_a};
  Panel another = multiple.panels.front();
  another.id = "second";
  another.name = "Second";
  another.items = std::vector<OverlayItem>{same_id_b};
  multiple.panels.push_back(std::move(another));
  expect(!validate_profile(multiple), "known IDs may repeat across different panels");

  auto raw_missing = json::parse(R"json({"kind":"future"})json");
  auto raw_wrong_id = json::parse(
      R"json({"kind":"future","id":{"guess":true},"cooldownId":{"bad":1},"flashHz":"no"})json");
  auto raw_duplicate_id = json::parse(
      R"json({"kind":"future","id":"same","cooldownId":false,"flashHz":[]})json");
  const auto opaque_root = [&] {
    auto root = root_object(base_profile(profile_schema_version));
    set_items(root, Array{std::get<Value>(raw_missing),
                          std::get<Value>(raw_wrong_id),
                          std::get<Value>(raw_duplicate_id)});
    return root;
  }();
  expect_valid(parse_root(opaque_root),
               "unknown kinds do not validate guessed IDs or payload field shapes");
  expect(items_path == "$.panels[0].items", "item validation paths use the item collection");

  auto bad_counter = base_profile();
  bad_counter.panels.front().items = std::vector<OverlayItem>{OverlayItem{
      .id = "bad", .content = CounterItem{"stacks", 0.0, CounterDisplay::number}}};
  expect(validation_path(bad_counter) ==
             std::optional<std::string>{"$.panels[0].items[0].max"},
         "counter maximum must be finite and positive");
  for (const double maximum : {0.0, 9.0, 1.5}) {
    auto pips = base_profile();
    pips.panels.front().items = std::vector<OverlayItem>{OverlayItem{
        .id = "pips", .content = CounterItem{"stacks", maximum, CounterDisplay::pips}}};
    expect(validation_path(pips) ==
               std::optional<std::string>{"$.panels[0].items[0].max"},
           "pips require integer max from one through eight");
  }
  auto pips_missing = base_profile();
  pips_missing.panels.front().items = std::vector<OverlayItem>{OverlayItem{
      .id = "pips", .content = CounterItem{"stacks", std::nullopt, CounterDisplay::pips}}};
  expect(validation_path(pips_missing) ==
             std::optional<std::string>{"$.panels[0].items[0].max"},
         "pips require an explicit maximum");
  for (const double maximum : {1.0, 8.0}) {
    auto pips = base_profile();
    pips.panels.front().items = std::vector<OverlayItem>{OverlayItem{
        .id = "pips", .content = CounterItem{"deferredIntegerVariable", maximum,
                                             CounterDisplay::pips}}};
    expect(!validate_profile(pips), "integer pips boundaries validate without sidecar lookup");
  }
  for (const double maximum : {std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
    auto invalid = base_profile();
    invalid.panels.front().items = std::vector<OverlayItem>{OverlayItem{
        .id = "counter", .content = CounterItem{"stacks", maximum,
                                                 CounterDisplay::number}}};
    expect(validation_path(invalid) ==
               std::optional<std::string>{"$.panels[0].items[0].max"},
           "non-finite in-memory counter maxima are rejected");
  }
  auto invalid_display = base_profile();
  invalid_display.panels.front().items = std::vector<OverlayItem>{OverlayItem{
      .id = "counter", .content = CounterItem{"stacks", std::nullopt,
          static_cast<CounterDisplay>(99)}}};
  expect(validation_path(invalid_display) ==
             std::optional<std::string>{"$.panels[0].items[0].display"},
         "in-memory invalid counter displays are rejected");
  for (const auto &maximum : {Value(std::int64_t{0}), Value(std::int64_t{-1})}) {
    auto item = counter_item();
    put(item, "max", maximum);
    expect_path(parse_root(root_with(std::move(item))),
                "$.panels[0].items[0].max", "counter maximum must be positive");
  }
  for (const double maximum : {1.0, 8.0}) {
    auto item = counter_item();
    put(item, "max", Value(maximum));
    put(item, "display", Value(std::string("pips")));
    expect_valid(parse_root(root_with(std::move(item))), "pips boundaries parse");
  }
  auto unknown_display = counter_item();
  put(unknown_display, "display", Value(std::string("dots")));
  expect_path(parse_root(root_with(std::move(unknown_display))),
              "$.panels[0].items[0].display", "unknown display enum is rejected");
  auto deferred_variable = counter_item("deferred", "not-declared-yet");
  expect_valid(parse_root(root_with(std::move(deferred_variable))),
               "variable declarations are checked cross-document in a later step");

  for (const double flash : {0.0, min_indicator_flash_hz, max_indicator_flash_hz}) {
    auto item = indicator_item();
    put(item, "flashHz", Value(flash));
    expect_valid(parse_root(root_with(std::move(item))), "supported indicator rates parse");
  }
  for (const double flash : {-0.1, 0.25, 4.1}) {
    auto item = indicator_item();
    put(item, "flashHz", Value(flash));
    expect_path(parse_root(root_with(std::move(item))),
                "$.panels[0].items[0].flashHz", "unsupported indicator rates fail at flashHz");
  }
  for (const double flash : {std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
    auto invalid_flash = base_profile();
    invalid_flash.panels.front().items = std::vector<OverlayItem>{OverlayItem{
        .id = "indicator", .content = IndicatorItem{"warning", flash}}};
    expect(validation_path(invalid_flash) ==
               std::optional<std::string>{"$.panels[0].items[0].flashHz"},
           "non-finite in-memory indicator rates are rejected");
  }

  const std::string invalid_utf8(1, static_cast<char>(0xFF));
  auto invalid_id = cooldown_item(invalid_utf8);
  expect_path(parse_root(root_with(std::move(invalid_id))),
              "$.panels[0].items[0].id", "item IDs require valid UTF-8");
  auto invalid_label = cooldown_item();
  put(invalid_label, "label", Value(invalid_utf8));
  expect_path(parse_root(root_with(std::move(invalid_label))),
              "$.panels[0].items[0].label", "item labels require valid UTF-8");
  auto invalid_ref = cooldown_item("ref", invalid_utf8);
  expect_path(parse_root(root_with(std::move(invalid_ref))),
              "$.panels[0].items[0].cooldownId", "item references require valid UTF-8");
  auto invalid_variable = counter_item("variable", invalid_utf8);
  expect_path(parse_root(root_with(std::move(invalid_variable))),
              "$.panels[0].items[0].variable", "item variables require valid UTF-8");
  const std::string byte512(512, 'x');
  const std::string byte513(513, 'x');
  auto label512 = cooldown_item();
  put(label512, "label", Value(byte512));
  expect_valid(parse_root(root_with(std::move(label512))),
               "512-byte labels are accepted");
  expect_valid(parse_root(root_with(counter_item("v512", byte512))),
               "512-byte variable names are accepted for deferred binding");
  expect_path(parse_root(root_with(counter_item("v513", byte513))),
              "$.panels[0].items[0].variable", "513-byte variable names are rejected");
  expect_valid(parse_root(root_with(timer_item("t512", "automation", "timerId", byte512))),
               "512-byte timer references are accepted for deferred binding");
  expect_path(parse_root(root_with(timer_item("t513", "automation", "timerId", byte513))),
              "$.panels[0].items[0].timerId", "513-byte timer references are rejected");
  auto long_reference = base_profile(profile_schema_version);
  long_reference.cooldowns.front().id = byte512;
  long_reference.bindings.front().cooldown_id = byte512;
  long_reference.panels.front().cooldown_ids = {byte512};
  long_reference.panels.front().items = std::vector<OverlayItem>{OverlayItem{
      .id = byte512, .content = CooldownItem{byte512}}};
  expect_valid(parse_profile_json(serialize_profile_json(long_reference)),
               "512-byte item IDs and existing cooldown references are accepted");
  expect_path(parse_root(root_with(cooldown_item(byte513, "ability"))),
              "$.panels[0].items[0].id", "513-byte item IDs are rejected");
  auto long_label = cooldown_item();
  put(long_label, "label", Value(byte513));
  expect_path(parse_root(root_with(std::move(long_label))),
              "$.panels[0].items[0].label", "513-byte labels are rejected");
  auto long_ref = cooldown_item("ref", byte513);
  expect_path(parse_root(root_with(std::move(long_ref))),
              "$.panels[0].items[0].cooldownId", "513-byte references are rejected");
  const std::string emoji("\xF0\x9F\x98\x80", 4);
  auto multibyte_id = cooldown_item([&] {
    std::string value;
    for (int i{}; i < 128; ++i) value += emoji;
    return value;
  }());
  expect_valid(parse_root(root_with(std::move(multibyte_id))),
               "UTF-8 byte limits count multibyte code points by bytes");
  auto too_many_emoji = cooldown_item([&] {
    std::string value;
    for (int i{}; i < 129; ++i) value += emoji;
    return value;
  }());
  expect_path(parse_root(root_with(std::move(too_many_emoji))),
              "$.panels[0].items[0].id", "multibyte IDs exceeding 512 bytes are rejected");
  auto empty_label = cooldown_item();
  put(empty_label, "label", Value(std::string{}));
  expect_valid(parse_root(root_with(std::move(empty_label))),
               "explicit empty labels are accepted");

  auto active_absent = parse_root(root_object(base_profile(profile_schema_version)));
  const auto *absent_active_profile = std::get_if<GameProfile>(&active_absent);
  expect(absent_active_profile &&
             absent_active_profile->cooldowns.front().active_duration == Milliseconds{0},
         "absent active duration defaults to zero");
  auto active_zero = root_object(base_profile(profile_schema_version));
  set_cooldown_field(active_zero, "activeDurationMs", Value(std::int64_t{0}));
  const auto zero_result = parse_root(std::move(active_zero));
  const auto *zero_profile = std::get_if<GameProfile>(&zero_result);
  expect(zero_profile && zero_profile->cooldowns.front().active_duration == Milliseconds{0} &&
             serialize_profile_json(*zero_profile).find("activeDurationMs") ==
                 std::string::npos,
         "explicit zero active duration defaults canonically to omission");
  auto active_positive = root_object(base_profile(profile_schema_version));
  set_cooldown_field(active_positive, "activeDurationMs", Value(std::int64_t{1000}));
  const auto positive_result = parse_root(std::move(active_positive));
  const auto *positive_profile = std::get_if<GameProfile>(&positive_result);
  expect(positive_profile && positive_profile->cooldowns.front().active_duration ==
             Milliseconds{1000} &&
             serialize_profile_json(*positive_profile).find("\"activeDurationMs\": 1000") !=
                 std::string::npos,
         "positive active duration parses and serializes");

  auto active_duration = root_object(base_profile(profile_schema_version));
  set_cooldown_field(active_duration, "activeDurationMs", Value(std::int64_t{-1}));
  expect_path(parse_root(std::move(active_duration)), "$.cooldowns[0].activeDurationMs",
              "negative active duration is rejected semantically");
  active_duration = root_object(base_profile(profile_schema_version));
  set_cooldown_field(active_duration, "activeDurationMs",
                     Value(max_cooldown_duration.count() + 1));
  expect_path(parse_root(std::move(active_duration)), "$.cooldowns[0].activeDurationMs",
              "active duration above the maximum is rejected");
  for (const auto &value : {Value(1.5), Value(std::string("1000"))}) {
    auto wrong_encoding = root_object(base_profile(profile_schema_version));
    set_cooldown_field(wrong_encoding, "activeDurationMs", value);
    expect_path(parse_root(std::move(wrong_encoding)),
                "$.cooldowns[0].activeDurationMs",
                "active duration requires an integral JSON number");
  }
  auto valid_duration = base_profile(profile_schema_version);
  valid_duration.cooldowns.front().active_duration = max_cooldown_duration;
  expect(!validate_profile(valid_duration) &&
             serialize_profile_json(valid_duration).find("\"activeDurationMs\": 86400000") !=
                 std::string::npos,
         "maximum active duration validates and serializes");
  auto zero_duration = base_profile();
  zero_duration.cooldowns.front().active_duration = Milliseconds{0};
  expect(serialize_profile_json(zero_duration).find("activeDurationMs") ==
             std::string::npos,
         "zero active duration is omitted from serialization");

  auto bad_active = base_profile();
  bad_active.cooldowns.front().active_duration = Milliseconds{-1};
  expect(validation_path(bad_active) ==
             std::optional<std::string>{"$.cooldowns[0].activeDurationMs"},
         "all in-memory profiles validate active duration bounds");
  auto active_timer_missing = root_with(
      timer_item("active", "cooldownActive", "cooldownId", "missing"));
  expect_path(parse_root(std::move(active_timer_missing)),
              "$.panels[0].items[0].cooldownId",
              "cooldown-active timers require an existing cooldown");
  auto active_timer_disabled = root_with(
      timer_item("active", "cooldownActive", "cooldownId", "ability"));
  expect_path(parse_root(std::move(active_timer_disabled)),
              "$.panels[0].items[0].cooldownId",
              "cooldown-active timers require positive active duration");
  auto with_active = base_profile(profile_schema_version);
  with_active.cooldowns.front().active_duration = Milliseconds{1};
  with_active.panels.front().items = std::vector<OverlayItem>{OverlayItem{
      .id = "active", .content = CooldownActiveTimerItem{"ability"}}};
  expect(!validate_profile(with_active), "resolved cooldown-active timers validate");

  auto limit32 = base_profile();
  limit32.panels.front().items = counter_items(max_overlay_items_per_panel);
  expect(!validate_profile(limit32), "32 typed items are accepted");
  auto limit33 = base_profile();
  limit33.panels.front().items = counter_items(max_overlay_items_per_panel + 1);
  expect(validation_path(limit33) ==
             std::optional<std::string>{"$.panels[0].items"},
         "33 typed items fail at the collection path");

  auto panel_limit64 = base_profile(profile_schema_version);
  for (std::size_t i = 1; i < max_typed_profile_panels; ++i) {
    auto panel = panel_limit64.panels.front();
    panel.id = "panel-" + std::to_string(i);
    panel.name = panel.id;
    panel_limit64.panels.push_back(std::move(panel));
  }
  expect(!validate_profile(panel_limit64), "64 schema-5 panels are accepted");
  auto panel_limit65 = panel_limit64;
  auto extra_panel = panel_limit65.panels.front();
  extra_panel.id = "panel-64";
  extra_panel.name = extra_panel.id;
  panel_limit65.panels.push_back(std::move(extra_panel));
  expect(validation_path(panel_limit65) == std::optional<std::string>{"$.panels"},
         "65 schema-5 panels fail at the collection path");

  auto legacy_panels = base_profile();
  for (std::size_t i = 1; i < max_typed_profile_panels + 1; ++i) {
    auto panel = legacy_panels.panels.front();
    panel.id = "legacy-" + std::to_string(i);
    panel.name = panel.id;
    legacy_panels.panels.push_back(std::move(panel));
  }
  const auto legacy_panel_bytes = serialize_profile_json(legacy_panels);
  const auto legacy_panel_round_trip = parse_profile_json(legacy_panel_bytes);
  expect(!validate_profile(legacy_panels) &&
             std::holds_alternative<GameProfile>(legacy_panel_round_trip) &&
             serialize_profile_json(std::get<GameProfile>(legacy_panel_round_trip)) ==
                 legacy_panel_bytes,
         "legacy schema-4 panel counts remain uncapped and byte-stable");

  auto legacy_cooldowns = base_profile();
  for (std::size_t i = 1; i < 33; ++i) {
    auto cooldown = legacy_cooldowns.cooldowns.front();
    cooldown.id = "ability-" + std::to_string(i);
    cooldown.name = cooldown.id;
    legacy_cooldowns.cooldowns.push_back(std::move(cooldown));
    legacy_cooldowns.panels.front().cooldown_ids.push_back(
        legacy_cooldowns.cooldowns.back().id);
  }
  const auto legacy_cooldown_bytes = serialize_profile_json(legacy_cooldowns);
  const auto legacy_cooldown_round_trip = parse_profile_json(legacy_cooldown_bytes);
  expect(!validate_profile(legacy_cooldowns) &&
             std::holds_alternative<GameProfile>(legacy_cooldown_round_trip) &&
             serialize_profile_json(std::get<GameProfile>(legacy_cooldown_round_trip)) ==
                 legacy_cooldown_bytes,
         "legacy panels with more than 32 cooldown IDs remain byte-stable");
}

void test_profile_items_legacy_compatibility() {
  for (std::uint32_t source_schema = 1; source_schema <= 4; ++source_schema) {
    auto root = root_object(base_profile(legacy_profile_schema_version));
    put(root, "schemaVersion", Value(static_cast<std::int64_t>(source_schema)));
    if (source_schema < 4) {
      auto cooldowns = root.at("cooldowns").as_array();
      auto cooldown = cooldowns.front().as_object();
      cooldown.erase("recovery");
      put(cooldown, "stackCharges", Value(false));
      cooldowns.front() = Value(std::move(cooldown));
      put(root, "cooldowns", Value(std::move(cooldowns)));
    }
    if (source_schema < 3) {
      remove_root_field(root, "modules");
      remove_root_field(root, "hidBindings");
    }
    if (source_schema < 4) remove_root_field(root, "cooldownGroups");
    const auto parsed = parse_root(std::move(root));
    const auto *profile = std::get_if<GameProfile>(&parsed);
    expect(profile && profile->schema_version == legacy_profile_schema_version &&
               !profile->panels.front().items.has_value() &&
               profile->panels.front().cooldown_ids ==
                   std::vector<std::string>{"ability"} &&
               serialize_profile_json(*profile).find("\"schemaVersion\": 4") !=
                   std::string::npos,
           "schema 1-4 fixtures migrate in memory to legacy schema 4");
  }

  const auto ordinary = base_profile();
  const auto ordinary_bytes = serialize_profile_json(ordinary);
  expect(ordinary.schema_version == legacy_profile_schema_version &&
             ordinary_bytes.find("\"schemaVersion\": 4") != std::string::npos &&
             ordinary_bytes.find("\"items\"") == std::string::npos &&
             ordinary_bytes.find("activeDurationMs") == std::string::npos,
         "ordinary profiles preserve schema-4 output without synthesizing items");
  const auto ordinary_again = parse_profile_json(ordinary_bytes);
  expect(std::holds_alternative<GameProfile>(ordinary_again) &&
             serialize_profile_json(std::get<GameProfile>(ordinary_again)) == ordinary_bytes,
         "ordinary schema-4 profile serialization remains stable after load/save");

  const Array non_cooldown_items{
      item_value(counter_item("counter", "stacks")),
      item_value(indicator_item("indicator", "warning")),
      item_value(timer_item("automation", "automation", "timerId", "stopped"))};
  for (int fallback_case{}; fallback_case < 3; ++fallback_case) {
    auto authoritative = root_object(base_profile(profile_schema_version));
    set_items(authoritative, non_cooldown_items);
    auto panel_fields = panel_object(authoritative);
    if (fallback_case == 0) {
      panel_fields.erase("cooldowns");
    } else if (fallback_case == 1) {
      put(panel_fields, "cooldowns", Value(Array{Value(std::string("ghost"))}));
    } else {
      put(panel_fields, "cooldowns", Value(std::string("invalid fallback shape")));
    }
    put_panel(authoritative, 0, std::move(panel_fields));
    const auto result = parse_root(std::move(authoritative));
    const auto *typed = std::get_if<GameProfile>(&result);
    expect(typed && typed->panels.front().items.has_value() &&
               typed->panels.front().cooldown_ids.empty(),
           "authoritative items ignore missing, mismatched, and invalid cooldown fallbacks");
    if (typed) {
      const auto encoded = serialize_profile_json(*typed);
      const auto wire = json::parse(encoded);
      const auto *serialized_root = std::get_if<Value>(&wire);
      const auto *panels = serialized_root == nullptr
          ? nullptr : serialized_root->find("panels");
      const auto *fallback = panels == nullptr || !panels->is_array()
          ? nullptr : panels->as_array().front().find("cooldowns");
      expect(fallback && fallback->is_array() && fallback->as_array().empty(),
             "typed panels save the exact empty cooldown projection");
    }
  }
  auto absent_items = root_object(base_profile(profile_schema_version));
  const auto absent_result = parse_root(std::move(absent_items));
  const auto *absent_profile = std::get_if<GameProfile>(&absent_result);
  expect(absent_profile && !absent_profile->panels.front().items.has_value() &&
             absent_profile->panels.front().cooldown_ids ==
                 std::vector<std::string>{"ability"},
         "items-absent schema-5 panels retain legacy cooldown behavior");
  auto non_array_items = root_object(base_profile(profile_schema_version));
  auto non_array_panel = panel_object(non_array_items);
  put(non_array_panel, "items", Value(std::string("not an array")));
  put_panel(non_array_items, 0, std::move(non_array_panel));
  expect_path(parse_root(std::move(non_array_items)), "$.panels[0].items",
              "a present non-array items field is an error, not fallback");

  auto opaque_root = root_object(base_profile(legacy_profile_schema_version));
  auto panel = panel_object(opaque_root);
  put(panel, "items", Value(std::string("not an item array")));
  put_panel(opaque_root, 0, std::move(panel));
  set_cooldown_field(opaque_root, "activeDurationMs",
                     Value(Object{{"future-shape", Value(Array{Value(true)})}}));
  const auto opaque_result = parse_root(std::move(opaque_root));
  const auto *opaque_profile = std::get_if<GameProfile>(&opaque_result);
  expect(opaque_profile && !opaque_profile->panels.front().items.has_value() &&
             opaque_profile->cooldowns.front().active_duration == Milliseconds{0} &&
             opaque_profile->panels.front().extra_fields.contains("items") &&
             opaque_profile->cooldowns.front().extra_fields.contains("activeDurationMs"),
         "schema-4 new field names and shapes remain opaque extensions");
  if (opaque_profile) {
    const auto bytes = serialize_profile_json(*opaque_profile);
    const auto round_trip = parse_profile_json(bytes);
    expect(std::holds_alternative<GameProfile>(round_trip) &&
               serialize_profile_json(std::get<GameProfile>(round_trip)) == bytes,
           "schema-4 opaque future shapes survive canonical saves byte-for-byte");
    auto promoted = *opaque_profile;
    promoted.schema_version = profile_schema_version;
    const auto promoted_error = validate_profile(promoted);
    expect(promoted_error && promoted_error->path ==
               "$.cooldowns[0].activeDurationMs" && serializer_throws(promoted),
           "explicit schema-5 promotion rejects cooldown extension collisions");
    promoted.cooldowns.front().extra_fields.erase("activeDurationMs");
    const auto panel_collision = validate_profile(promoted);
    expect(panel_collision && panel_collision->path == "$.panels[0].items" &&
               serializer_throws(promoted),
           "explicit schema-5 promotion rejects panel extension collisions");
  }

  auto schema4 = base_profile();
  auto migrated = parse_profile_json(serialize_profile_json(schema4));
  auto *migrated_profile = std::get_if<GameProfile>(&migrated);
  expect(migrated_profile && migrated_profile->schema_version ==
             legacy_profile_schema_version,
         "fresh legacy model stays schema 4 until schema-5 features are enabled");

  auto no_fallback = root_object(base_profile(profile_schema_version));
  set_items(no_fallback, Array{});
  const auto empty_items = parse_root(std::move(no_fallback));
  expect_path(empty_items, "$.panels[0].items",
              "present but empty typed item lists do not fall back to cooldowns");

  GameProfile fresh;
  expect(fresh.schema_version == legacy_profile_schema_version &&
             profile_serialization_schema(fresh) == legacy_profile_schema_version,
         "fresh GameProfile defaults to legacy schema 4");
  auto explicit_v5 = base_profile(profile_schema_version);
  expect(profile_serialization_schema(explicit_v5) == profile_schema_version,
         "explicit schema 5 remains selected without schema-5 features");
  explicit_v5.panels.front().items.reset();
  explicit_v5.cooldowns.front().active_duration = Milliseconds{0};
  expect(profile_serialization_schema(explicit_v5) == profile_schema_version,
         "removing typed features does not downgrade an explicit schema 5 profile");
  auto promoted_by_item = base_profile();
  promoted_by_item.panels.front().items = std::vector<OverlayItem>{OverlayItem{
      .id = "cooldown", .content = CooldownItem{"ability"}}};
  expect(profile_serialization_schema(promoted_by_item) == profile_schema_version &&
             serialize_profile_json(promoted_by_item).find("\"schemaVersion\": 5") !=
                 std::string::npos,
         "adding a typed item promotes a default model to schema 5");
  auto promoted_by_duration = base_profile();
  promoted_by_duration.cooldowns.front().active_duration = Milliseconds{1};
  expect(profile_serialization_schema(promoted_by_duration) == profile_schema_version &&
             serialize_profile_json(promoted_by_duration).find("\"schemaVersion\": 5") !=
                 std::string::npos,
         "adding active duration promotes a default model to schema 5");

  namespace detail = layerc::core::detail;
  const auto item_names = detail::known_profile_fields(detail::ProfileObject::item);
  expect(item_names.size() == profile_keys::item::fields.size() &&
             detail::known_profile_fields(detail::ProfileObject::item, 4).empty(),
         "the single item registry defines current and pre-v5 classifications");
  Object declared;
  for (const auto name : item_names) declared.emplace(std::string(name), Value(nullptr));
  declared.emplace("futureUnrecognized", Value(true));
  const auto opaque = detail::unknown_profile_fields(Value(std::move(declared)),
                                                      detail::ProfileObject::item);
  expect(opaque.size() == 1 && opaque.contains("futureUnrecognized"),
         "default classification excludes every declared item field");
  const auto panel_opaque = detail::unknown_profile_fields(
      Value(Object{{"items", Value(std::string("future"))}}),
      detail::ProfileObject::panel, legacy_profile_schema_version);
  const auto cooldown_opaque = detail::unknown_profile_fields(
      Value(Object{{"activeDurationMs", Value(true)}}),
      detail::ProfileObject::cooldown, legacy_profile_schema_version);
  expect(panel_opaque.contains("items") && cooldown_opaque.contains("activeDurationMs"),
         "schema 4 classification retains the new names as opaque extensions");
  bool all_since_v5 = true;
  for (const auto &field : profile_keys::item::fields)
    all_since_v5 = all_since_v5 && field.since_schema == profile_schema_version;
  const auto panel_items = std::ranges::find_if(profile_keys::panel::fields,
      [](const auto &field) { return field.name == "items"; });
  const auto cooldown_active = std::ranges::find_if(profile_keys::cooldown::fields,
      [](const auto &field) { return field.name == "activeDurationMs"; });
  expect(all_since_v5 && panel_items != profile_keys::panel::fields.end() &&
             panel_items->since_schema == profile_schema_version &&
             cooldown_active != profile_keys::cooldown::fields.end() &&
             cooldown_active->since_schema == profile_schema_version,
         "all schema-5 names and item fields are declared once with their version");

  auto item_extensions = base_profile();
  item_extensions.panels.front().items = std::vector<OverlayItem>{OverlayItem{
      .id = "known", .content = CooldownItem{"ability"}}};
  for (const auto name : profile_keys::item::names) {
    auto collision = item_extensions;
    collision.panels.front().items->front().extra_fields.emplace(
        std::string(name), Value(nullptr));
    expect(validation_path(collision) == std::optional<std::string>{
               "$.panels[0].items[0]." + std::string(name)} &&
               serializer_throws(collision),
           "known item extensions collide with every declared item field");
  }

  auto invalid_opaque = base_profile();
  const std::string malformed_utf8(1, static_cast<char>(0xFF));
  invalid_opaque.panels.front().items = std::vector<OverlayItem>{OverlayItem{
      .content = OpaqueOverlayItem{Value(Object{
          {"kind", Value(std::string("future"))},
          {"bad", Value(malformed_utf8)}})}}};
  expect(validation_path(invalid_opaque).has_value(),
         "invalid UTF-8 in opaque item data is rejected");
  invalid_opaque.panels.front().items->front().content = OpaqueOverlayItem{Value(Object{
      {"kind", Value(std::string("future"))},
      {"bad", Value(std::numeric_limits<double>::infinity())}})};
  expect(validation_path(invalid_opaque).has_value(),
         "non-finite opaque item data is rejected");
  Value deep(nullptr);
  for (int i{}; i < 124; ++i) deep = Value(Array{std::move(deep)});
  invalid_opaque.panels.front().items->front().content = OpaqueOverlayItem{Value(Object{
      {"kind", Value(std::string("future"))}, {"deep", std::move(deep)}})};
  expect(validation_path(invalid_opaque).has_value(),
         "opaque item nesting uses the existing JSON depth bound");
  invalid_opaque.panels.front().items->front().content = OpaqueOverlayItem{Value(Object{
      {"kind", Value(std::string("future"))}})};
  invalid_opaque.panels.front().items->front().id = "typed-id";
  expect(validation_path(invalid_opaque) == std::optional<std::string>{
             "$.panels[0].items[0].id"},
         "opaque variants reject accidental typed common members");
}

} // namespace layerc::test
