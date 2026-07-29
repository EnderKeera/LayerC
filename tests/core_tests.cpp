#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/input_matcher.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/runtime_instances.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

void test_cooldown_charges() {
  using namespace std::chrono_literals;
  using namespace layerc::core;

  const std::vector<CooldownDefinition> definitions{CooldownDefinition{
      .id = "dash",
      .name = "Dash",
      .duration = 5s,
      .max_charges = 2,
      .retrigger = RetriggerPolicy::ignore,
      .stack_charges = true,
  }};
  CooldownEngine engine(definitions);

  const auto start = TimePoint{};
  expect(engine.trigger("dash", start), "first charge should trigger");
  expect(engine.trigger("dash", start + 1s), "second charge should trigger");
  expect(!engine.trigger("dash", start + 2s),
         "empty cooldown should ignore trigger");

  auto state = engine.snapshot(start + 4s).front();
  expect(state.available_charges == 0,
         "charges should remain empty before first recharge");
  state = engine.snapshot(start + 5s).front();
  expect(state.available_charges == 1,
         "first charge should recover after duration");
  state = engine.snapshot(start + 10s).front();
  expect(state.available_charges == 2 && !state.next_charge_at.has_value(),
         "sequential charges should fully recover");
}

void test_non_stacking_cooldown() {
  using namespace std::chrono_literals;
  using namespace layerc::core;

  const std::vector<CooldownDefinition> definitions{CooldownDefinition{
      .id = "dash",
      .name = "Dash",
      .duration = 5s,
      .max_charges = 2,
      .retrigger = RetriggerPolicy::ignore,
  }};
  CooldownEngine engine(definitions);
  const auto start = TimePoint{};
  expect(engine.trigger("dash", start),
         "non-stacking cooldown should trigger initially");
  expect(!engine.trigger("dash", start + 1s),
         "non-stacking cooldown should ignore early retrigger");

  auto state = engine.snapshot(start + 5s).front();
  expect(state.available_charges == 2 && !state.next_charge_at.has_value(),
         "non-stacking cooldown should be ready after one timer");
  expect(engine.trigger("dash", start + 5s),
         "non-stacking cooldown should trigger after expiry");
  expect(!engine.trigger("dash", start + 6s),
         "non-stacking cooldown should not queue another timer");
}

void test_restart_policy() {
  using namespace std::chrono_literals;
  using namespace layerc::core;

  const std::vector<CooldownDefinition> definitions{CooldownDefinition{
      .id = "blink",
      .name = "Blink",
      .duration = 5s,
      .max_charges = 1,
      .retrigger = RetriggerPolicy::restart,
  }};
  CooldownEngine engine(definitions);
  const auto start = TimePoint{};
  expect(engine.trigger("blink", start),
         "initial restart cooldown trigger should work");
  expect(engine.trigger("blink", start + 4s),
         "restart policy should accept retrigger");
  expect(engine.snapshot(start + 5s).front().available_charges == 0,
         "restart should delay recharge");
  expect(engine.snapshot(start + 9s).front().available_charges == 1,
         "restarted cooldown should eventually recover");
}

void test_snapshot_reuse() {
  using namespace std::chrono_literals;
  using namespace layerc::core;

  const std::vector<CooldownDefinition> definitions{CooldownDefinition{
      .id = "dash",
      .name = "Dash",
      .duration = 5s,
  }};
  CooldownEngine engine(definitions);
  std::vector<CooldownState> states;
  engine.snapshot(TimePoint{}, states);
  const auto capacity = states.capacity();
  const auto *storage = states.data();
  engine.snapshot(TimePoint{} + 1s, states);
  expect(states.capacity() == capacity && states.data() == storage,
         "cooldown snapshots should reuse their caller-owned buffer");
}

void test_input_chord_edges() {
  using namespace layerc::core;
  const InputToken shift{InputDevice::keyboard, 16, 0};
  const InputToken e{InputDevice::keyboard, 69, 0};
  const std::vector<InputBinding> bindings{
      InputBinding{.cooldown_id = "dash", .chord = InputChord{{shift, e}}}};
  InputMatcher matcher(bindings);

  expect(matcher.process(InputEvent{shift, true}).empty(),
         "partial chord should not trigger");
  auto triggers = matcher.process(InputEvent{e, true});
  expect(triggers.size() == 1 && triggers.front() == "dash",
         "completed chord should trigger once");
  expect(matcher.process(InputEvent{e, true}).empty(),
         "held chord should not repeat");
  expect(matcher.process(InputEvent{e, false}).empty(),
         "release should only rearm chord");
  expect(matcher.process(InputEvent{e, true}).size() == 1,
         "re-pressed chord should trigger again");
}

constexpr std::string_view valid_profile = R"json(
{
  "schemaVersion": 1,
  "id": "game",
  "name": "Game",
  "game": { "executableName": "game.exe" },
  "cooldowns": [
    { "id": "dash", "name": "Dash", "durationMs": 5000, "maxCharges": 2, "stackCharges": true }
  ],
  "panels": [
    { "id": "main", "name": "Main", "layout": "row", "style": "bar",
      "bar": { "width": 360, "height": 30, "fillColor": "#50BAC8",
        "backgroundColor": "#202A31", "borderColor": "#80D8E8",
        "textColor": "#F1F5F7", "hideWhenReady": true }, "anchorX": 0.5,
      "anchorY": 0.75, "cooldowns": ["dash"] }
  ],
  "bindings": [
    { "cooldownId": "dash", "chord": [{"device": "keyboard", "code": 69}] }
  ]
}
)json";

std::string profile_with_token(const std::string_view device, const int code) {
  return R"json({"schemaVersion":1,"id":"game","name":"Game","game":{"executableName":"game.exe"},"cooldowns":[{"id":"dash","name":"Dash","durationMs":5000}],"panels":[{"id":"main","name":"Main","cooldowns":["dash"]}],"bindings":[{"cooldownId":"dash","chord":[{"device":")json" +
         std::string(device) + R"json(","code":)json" + std::to_string(code) +
         R"json(}]}]})json";
}

void test_profile_parser() {
  using namespace layerc::core;

  auto result = parse_profile_json(valid_profile);
  expect(std::holds_alternative<GameProfile>(result),
         "valid profile should parse");

  if (const auto *profile = std::get_if<GameProfile>(&result)) {
    expect(profile->cooldowns.size() == 1, "profile should contain cooldown");
    expect(profile->panels.front().cooldown_ids.front() == "dash",
           "panel reference should resolve");
    expect(profile->bindings.front().chord.tokens.front().code == 69,
           "binding code should parse");
    expect(profile->panels.front().style == PanelStyle::bar,
           "panel style should parse");
    expect(profile->panels.front().bar.width == 360 &&
               profile->panels.front().bar.height == 30,
           "bar dimensions should parse");
    expect(profile->panels.front().bar.fill_color == 0x50BAC8 &&
               profile->panels.front().bar.hide_when_ready,
           "bar appearance should parse");
    expect(profile->cooldowns.front().stack_charges,
           "stackCharges should parse");
  }

  const std::string oversized(max_profile_json_bytes + 1U, 'x');
  result = parse_profile_json(oversized);
  expect(std::holds_alternative<ProfileError>(result),
         "oversized profiles should be rejected before parsing");

  result = parse_profile_json(
      R"json({"schemaVersion":1,"id":"x","name":"x","game":{"executableName":"x.exe"},"cooldowns":[],"panels":[],"bindings":[]})json");
  expect(std::holds_alternative<ProfileError>(result),
         "empty cooldown list should fail validation");

  result =
      parse_profile_json(R"json({"schemaVersion":1,"schemaVersion":1})json");
  expect(std::holds_alternative<ProfileError>(result),
         "duplicate JSON object keys should be rejected");

  result = parse_profile_json(R"json({
      "schemaVersion":1,"id":"global","name":"Global","game":{"global":true},
      "cooldowns":[{"id":"dash","name":"Dash","durationMs":1000}],
      "panels":[{"id":"main","name":"Main","cooldowns":["dash"]}],
      "bindings":[{"cooldownId":"dash","chord":[{"device":"keyboard","code":69}]}]
    })json");
  expect(std::holds_alternative<GameProfile>(result),
         "global test profile should parse without executable");
  if (const auto *profile = std::get_if<GameProfile>(&result)) {
    expect(profile->game.global, "global profile flag should be retained");
  }
}

void test_profile_input_code_validation() {
  using namespace layerc::core;

  auto result = parse_profile_json(profile_with_token("mouse", 7));
  expect(std::holds_alternative<GameProfile>(result),
         "highest emitted mouse code should parse");
  result = parse_profile_json(profile_with_token("mouse", 8));
  expect(std::holds_alternative<ProfileError>(result),
         "non-emittable mouse code should fail validation");

  result = parse_profile_json(profile_with_token("controller", 18));
  expect(std::holds_alternative<GameProfile>(result),
         "highest emitted controller code should parse");
  result = parse_profile_json(profile_with_token("controller", 10));
  expect(std::holds_alternative<GameProfile>(result),
         "highest pre-reserved XInput button code should parse");
  result = parse_profile_json(profile_with_token("controller", 11));
  expect(std::holds_alternative<ProfileError>(result),
         "reserved XInput button code 11 should fail validation");
  result = parse_profile_json(profile_with_token("controller", 12));
  expect(std::holds_alternative<ProfileError>(result),
         "reserved XInput button code 12 should fail validation");
  result = parse_profile_json(profile_with_token("controller", 13));
  expect(std::holds_alternative<GameProfile>(result),
         "first post-reserved XInput button code should parse");
  result = parse_profile_json(profile_with_token("controller", 19));
  expect(std::holds_alternative<ProfileError>(result),
         "non-emittable controller code should fail validation");
}

void test_profile_game_matcher_utf8_validation() {
  using namespace layerc::core;

  std::string malformed =
      R"json({"schemaVersion":1,"id":"game","name":"Game","game":{"executablePath":")json";
  malformed.push_back(static_cast<char>(0xFF));
  malformed +=
      R"json("},"cooldowns":[{"id":"dash","name":"Dash","durationMs":5000}],"panels":[{"id":"main","name":"Main","cooldowns":["dash"]}],"bindings":[{"cooldownId":"dash","chord":[{"device":"keyboard","code":69}]}]})json";
  auto result = parse_profile_json(malformed);
  expect(std::holds_alternative<ProfileError>(result),
         "invalid UTF-8 game matcher should fail validation");

  result = parse_profile_json(
      R"json({"schemaVersion":1,"id":"game","name":"Game","game":{"executableName":"g\u00E1me.exe"},"cooldowns":[{"id":"dash","name":"Dash","durationMs":5000}],"panels":[{"id":"main","name":"Main","cooldowns":["dash"]}],"bindings":[{"cooldownId":"dash","chord":[{"device":"keyboard","code":69}]}]})json");
  expect(std::holds_alternative<GameProfile>(result),
         "valid UTF-8 game matcher should parse");
}

std::string insert_invalid_utf8(const std::string_view source,
                                const std::string_view value_prefix) {
  std::string malformed(source);
  const auto position = malformed.find(value_prefix);
  if (position != std::string::npos) {
    malformed.insert(position + value_prefix.size(), 1,
                     static_cast<char>(0xFF));
  }
  return malformed;
}

void test_profile_identity_utf8_validation() {
  using namespace layerc::core;

  for (const auto prefix : {std::string_view{R"json("id": ")json"},
                            std::string_view{R"json("name": "Dash)json"},
                            std::string_view{R"json("id": "main)json"}}) {
    const auto result = parse_profile_json(insert_invalid_utf8(valid_profile, prefix));
    expect(std::holds_alternative<ProfileError>(result),
           "invalid UTF-8 in persisted profile names and IDs should be rejected");
  }

  auto unicode_profile = std::string(valid_profile);
  const auto name = unicode_profile.find(R"json("name": "Game")json");
  if (name != std::string::npos) {
    unicode_profile.replace(name, std::string_view{R"json("name": "Game")json"}.size(),
                            R"json("name": "Gam\u00E9")json");
  }
  expect(std::holds_alternative<GameProfile>(parse_profile_json(unicode_profile)),
         "valid Unicode profile names should remain supported");
}
void test_profile_serialization() {
  using namespace layerc::core;

  const auto parsed = parse_profile_json(valid_profile);
  const auto *source = std::get_if<GameProfile>(&parsed);
  if (source == nullptr) {
    expect(false, "profile should parse before serialization");
    return;
  }

  auto profile = *source;
  profile.name = "Game \"profile\"";
  profile.cooldowns.front().name = "Dash\nReady";
  const auto serialized = serialize_profile_json(profile);
  const auto reparsed = parse_profile_json(serialized);
  const auto *round_tripped = std::get_if<GameProfile>(&reparsed);
  if (round_tripped == nullptr) {
    expect(false, "serialized profile should parse as a valid profile");
    return;
  }

  expect(round_tripped->id == profile.id && round_tripped->name == profile.name,
         "serializer should preserve profile identity and escaped text");
  expect(round_tripped->game.executable_name == profile.game.executable_name,
         "serializer should preserve application matcher");
  expect(round_tripped->cooldowns.size() == 1 &&
             round_tripped->cooldowns.front().name == "Dash\nReady" &&
             round_tripped->cooldowns.front().duration ==
                 std::chrono::milliseconds(5000),
         "serializer should preserve cooldown values");
  expect(round_tripped->panels.size() == 1 &&
             round_tripped->panels.front().style == PanelStyle::bar &&
             round_tripped->panels.front().bar.width == 360 &&
             round_tripped->panels.front().bar.hide_when_ready,
         "serializer should preserve bar appearance");
  expect(round_tripped->bindings.size() == 1 &&
             round_tripped->bindings.front().chord.tokens.size() == 1 &&
             round_tripped->bindings.front().chord.tokens.front().code == 69,
         "serializer should preserve bindings");
}
void test_profile_enabled_and_independent_bar_placement() {
  using namespace layerc::core;

  const auto legacy = parse_profile_json(valid_profile);
  const auto *legacy_profile = std::get_if<GameProfile>(&legacy);
  expect(legacy_profile != nullptr && legacy_profile->enabled,
         "legacy profiles should default enabled");

  constexpr std::string_view independent_profile = R"json(
    {
      "schemaVersion": 1,
      "id": "two-bars",
      "name": "Two independent bars",
      "enabled": false,
      "game": { "executableName": "game.exe" },
      "cooldowns": [
        { "id": "dash", "name": "Dash", "durationMs": 4000 },
        { "id": "blink", "name": "Blink", "durationMs": 9000 }
      ],
      "panels": [
        { "id": "dash-bar", "name": "Dash", "style": "bar",
          "bar": { "width": 210, "height": 24, "hideWhenReady": false },
          "anchorX": 0.12, "anchorY": 0.81, "cooldowns": ["dash"] },
        { "id": "blink-bar", "name": "Blink", "style": "bar",
          "bar": { "width": 340, "height": 32, "hideWhenReady": true },
          "anchorX": 0.87, "anchorY": 0.19, "cooldowns": ["blink"] }
      ],
      "bindings": [
        { "cooldownId": "dash", "chord": [{ "device": "keyboard", "code": 32 }] },
        { "cooldownId": "blink", "chord": [{ "device": "keyboard", "code": 48 }] }
      ]
    }
  )json";

  const auto parsed = parse_profile_json(independent_profile);
  const auto *profile = std::get_if<GameProfile>(&parsed);
  expect(profile != nullptr && !profile->enabled,
         "enabled=false should parse for a disabled profile");
  if (profile == nullptr) {
    return;
  }
  expect(profile->panels.size() == 2 &&
             profile->panels[0].cooldown_ids.size() == 1 &&
             profile->panels[1].cooldown_ids.size() == 1,
         "independent bars should retain one cooldown per panel");
  expect(profile->panels[0].anchor_x == 0.12 &&
             profile->panels[0].anchor_y == 0.81 &&
             profile->panels[1].anchor_x == 0.87 &&
             profile->panels[1].anchor_y == 0.19,
         "independent bars should retain normalized placement");

  const auto round_trip = parse_profile_json(serialize_profile_json(*profile));
  const auto *serialized = std::get_if<GameProfile>(&round_trip);
  expect(serialized != nullptr && !serialized->enabled &&
             serialized->panels.size() == 2 &&
             serialized->panels[0].bar.width == 210 &&
             serialized->panels[1].bar.width == 340 &&
             serialized->panels[0].anchor_y == 0.81 &&
             serialized->panels[1].anchor_x == 0.87,
         "serializer should preserve enabled state and per-bar placement");
}
void test_runtime_instance_tracking() {
  using namespace layerc::core;
  RuntimeInstanceTracker tracker;
  const RuntimeInstanceKey first{.profile_key = L"game-profile", .window = 101};
  const RuntimeInstanceKey second{.profile_key = L"game-profile", .window = 202};
  const RuntimeInstanceKey same_window_other_profile{
      .profile_key = L"game-profile-copy", .window = 202};

  const auto added =
      tracker.reconcile({first, second, same_window_other_profile});
  expect(added.added.size() == 3 && added.removed.empty() &&
             tracker.size() == 3,
         "matching profiles and windows should create independent runtime instances");
  const auto foreground_second = tracker.foreground(202);
  expect(foreground_second.size() == 2 &&
             std::all_of(foreground_second.begin(), foreground_second.end(),
                         [](const RuntimeInstanceKey &key) {
                           return key.window == 202;
                         }),
         "foreground input should route to every profile matching the foreground window");

  const std::vector<CooldownDefinition> cooldowns{{.id = "dash", .name = "Dash",
                                                    .duration = std::chrono::seconds(5)}};
  CooldownEngine first_engine(cooldowns);
  CooldownEngine second_engine(cooldowns);
  expect(first_engine.trigger("dash", TimePoint{}),
         "first instance should accept its own cooldown trigger");
  expect(first_engine.snapshot(TimePoint{}).front().next_charge_at.has_value() &&
             !second_engine.snapshot(TimePoint{}).front().next_charge_at.has_value(),
         "two matching instances should retain separate cooldown timelines");

  const auto removed = tracker.reconcile({second, same_window_other_profile});
  expect(removed.removed.size() == 1 && removed.removed.front() == first &&
             tracker.size() == 2 && tracker.foreground(101).empty(),
         "closing or minimizing a matched window should retire its runtime instance");
}
void test_json_unicode_and_errors() {
  using namespace layerc::core::json;

  auto result = parse(R"json({"name":"Layer\u0043","value":1.5})json");
  expect(std::holds_alternative<Value>(result),
         "valid unicode JSON should parse");
  if (const auto *value = std::get_if<Value>(&result)) {
    expect(value->find("name") != nullptr &&
               value->find("name")->as_string() == "LayerC",
           "unicode escape should decode to UTF-8");
  }

  result = parse("[1,]");
  expect(std::holds_alternative<ParseError>(result),
         "trailing array comma should fail");
}

void test_json_nesting_limit() {
  using namespace layerc::core::json;

  std::string excessive(200, '[');
  excessive += '0';
  excessive.append(200, ']');
  expect(std::holds_alternative<ParseError>(parse(excessive)),
         "excessively nested JSON arrays should return a parse error");

  std::string excessive_objects;
  for (int depth = 0; depth < 200; ++depth) excessive_objects += R"json({"x":)json";
  excessive_objects += '0';
  excessive_objects.append(200, '}');
  expect(std::holds_alternative<ParseError>(parse(excessive_objects)),
         "excessively nested JSON objects should return a parse error");

  std::string ordinary(32, '[');
  ordinary += '0';
  ordinary.append(32, ']');
  expect(std::holds_alternative<Value>(parse(ordinary)),
         "ordinary nested JSON should remain supported");
}
} // namespace

int main() {
  test_cooldown_charges();
  test_non_stacking_cooldown();
  test_restart_policy();
  test_snapshot_reuse();
  test_input_chord_edges();
  test_profile_parser();
  test_profile_input_code_validation();
  test_profile_game_matcher_utf8_validation();
  test_profile_identity_utf8_validation();
  test_profile_serialization();
  test_profile_enabled_and_independent_bar_placement();
  test_runtime_instance_tracking();
  test_json_unicode_and_errors();
  test_json_nesting_limit();

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All LayerC core tests passed\n";
  return EXIT_SUCCESS;
}
