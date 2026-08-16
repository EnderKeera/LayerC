#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/profile.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <variant>

namespace layerc::test {
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
    expect(profile->schema_version == 4,
           "legacy base profiles should migrate to v4 in memory");
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
               profile->panels.front().bar.show_text &&
               profile->panels.front().bar.hide_when_ready,
           "bar appearance should parse with legacy showText default");
    expect(profile->cooldowns.front().recovery == RecoveryMode::sequential,
           "schema <4 stackCharges should derive sequential recovery");
    expect(serialize_profile_json(*profile).find("\"schemaVersion\": 4") !=
               std::string::npos,
           "saving a migrated base profile should emit schema v4");
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
  profile.panels.front().bar.show_text = false;
  profile.panels.front().anchor_x = 0.12345678901234566;
  profile.panels.front().anchor_y = 0.87654321098765431;
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
             !round_tripped->panels.front().bar.show_text &&
             round_tripped->panels.front().bar.hide_when_ready &&
             round_tripped->panels.front().anchor_x ==
                 profile.panels.front().anchor_x &&
             round_tripped->panels.front().anchor_y ==
                 profile.panels.front().anchor_y,
         "serializer should preserve bar appearance and exact normalized placement");
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
          "bar": { "width": 210, "height": 24, "showText": false, "hideWhenReady": false },
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
             !serialized->panels[0].bar.show_text &&
             serialized->panels[1].bar.width == 340 &&
             serialized->panels[0].anchor_y == 0.81 &&
             serialized->panels[1].anchor_x == 0.87,
         "serializer should preserve enabled state and per-bar placement");
}

void test_profile_schema3_hid() {
  using namespace layerc::core;
  constexpr std::string_view source = R"json(
  {
    "schemaVersion": 3,
    "id": "hid-game",
    "name": "HID Game",
    "enabled": true,
    "game": {"executableName":"game.exe"},
    "visualRuntime": "isolated",
    "modules": [],
    "cooldowns": [{"id":"dash","name":"Dash","durationMs":1000}],
    "hidBindings": [{
      "id": "pedal",
      "device": {
        "vid": 4660, "pid": 22136, "version": 1,
        "usagePage": 1, "usage": 4,
        "descriptorHash": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "stableSlot": 1
      },
      "control": {
        "reportId": 2, "usagePage": 1, "usage": 48,
        "linkCollection": 0, "activation": "axisPositive",
        "value": 0, "threshold": 0.7, "hysteresis": 0.1
      },
      "allowSystemClass": false
    }],
    "panels": [{"id":"main","name":"Main","cooldowns":["dash"]}],
    "bindings": [{
      "cooldownId":"dash",
      "chord":[{"device":"hid","bindingId":"pedal"}]
    }]
  })json";
  const auto parsed = parse_profile_json(source);
  const auto *profile = std::get_if<GameProfile>(&parsed);
  expect(profile != nullptr && profile->schema_version == 4 &&
             profile->visual_runtime == VisualRuntime::isolated &&
             profile->hid_bindings.size() == 1 &&
             profile->bindings.front().chord.tokens.front().device ==
                 InputDevice::generic_hid &&
             profile->bindings.front().chord.tokens.front().code == 1,
         "schema 3 should resolve persisted HID binding IDs to compact tokens");
  if (profile != nullptr) {
    const auto round_trip = parse_profile_json(serialize_profile_json(*profile));
    expect(std::holds_alternative<GameProfile>(round_trip) &&
               std::get<GameProfile>(round_trip) == *profile,
           "schema-3 HID descriptors should round trip deterministically");
  }

  auto system_class = std::string(source);
  const auto usage = system_class.find(R"json("usage": 4)json");
  if (usage != std::string::npos)
    system_class.replace(usage, std::string_view(R"json("usage": 4)json").size(),
                         R"json("usage": 6)json");
  expect(std::holds_alternative<ProfileError>(
             parse_profile_json(system_class)),
         "keyboard-class HID should require explicit per-binding opt-in");

  auto unknown_module = std::string(source);
  const auto modules = unknown_module.find(R"json("modules": [])json");
  if (modules != std::string::npos)
    unknown_module.replace(
        modules, std::string_view(R"json("modules": [])json").size(),
        R"json("modules": [{"kind":"plugin","schemaVersion":1}])json");
  expect(std::holds_alternative<ProfileError>(
             parse_profile_json(unknown_module)),
         "schema 3 should reject unknown module kinds");
}

} // namespace layerc::test
