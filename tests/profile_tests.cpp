#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/core/panel_geometry.hpp"
#include "layerc/core/hid.hpp"
#include "layerc/core/json.hpp"
#include "../src/core/profile_extensions.hpp"
#include <limits>

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

  const std::string oversized(max_profile_json_bytes + 1U, ' ');
  result = parse_profile_json(oversized);
  const auto *oversized_error = std::get_if<ProfileError>(&result);
  expect(oversized_error && oversized_error->path == "$" &&
             oversized_error->message.find("exceeds") != std::string::npos,
         "oversized profiles should be rejected by the bounded parser");

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
  expect(serialize_profile_json(*round_tripped) == serialized,
         "serializer should emit byte-stable JSON after a round trip");

  auto wide_profile = profile;
  json::Value::Object wide_fields;
  for (std::size_t index = 0; index < 18'000; ++index)
    wide_fields.emplace("u" + std::to_string(index), json::Value(nullptr));
  wide_profile.extra_fields["futureWide"] = json::Value(std::move(wide_fields));
  const auto compact_profile = serialize_profile_json(wide_profile);
  const auto compact_value = json::parse(compact_profile);
  const auto *compact_root = std::get_if<json::Value>(&compact_value);
  if (!expect(compact_root != nullptr,
              "compact oversized-pretty profile parses as JSON")) return;
  std::string pretty_profile;
  json::append_value(pretty_profile, *compact_root, 2);
  pretty_profile += "\r\n";
  const auto compact_profile_result = parse_profile_json(compact_profile);
  const auto *compact_round_trip =
      std::get_if<GameProfile>(&compact_profile_result);
  expect(pretty_profile.size() > max_profile_json_bytes &&
             compact_profile.size() <= max_profile_json_bytes &&
             compact_round_trip != nullptr &&
             compact_round_trip->extra_fields.at("futureWide") ==
                 wide_profile.extra_fields.at("futureWide"),
         "oversized pretty output falls back to complete compact JSON within the wire limit");

  // Opaque data is value-owned, not tied to JSON text or array positions.
  auto extended = profile;
  extended.schema_version = profile_schema_version;
  const auto payload = json::parse(R"json({"future":{"n":9223372036854775807,"u":18446744073709551615,"f":0.12345678901234567890123456789,"a":[null,true,1.25,"opaque",-0]}})json");
  const auto extras = std::get<json::Value>(payload).as_object();
  extended.extra_fields = extras;
  extended.game.extra_fields = extras;
  extended.cooldowns.front().extra_fields = extras;
  auto other = extended.cooldowns.front();
  other.id = "other";
  extended.cooldowns.push_back(other);
  extended.cooldown_groups.push_back({.id = "group", .name = "Group",
      .cooldown_ids = {"dash", "other"}, .extra_fields = extras});
  extended.modules.push_back({.kind = ProfileModuleKind::visual,
      .schema_version = 2, .extra_fields = extras});
  extended.panels.front().extra_fields = extras;
  extended.panels.front().bar.extra_fields = extras;
  extended.bindings.front().extra_fields = extras;
  extended.bindings.front().token_extra_fields.emplace(
      extended.bindings.front().chord.tokens.front(), extras);
  HidControlBinding hid;
  hid.id = "unused";
  hid.device.stable_slot = 1;
  hid.device.descriptor_hash = std::string(64, 'a');
  hid.device.usage_page = 0xFF00;
  hid.device.usage = 1;
  hid.usage_page = 0xFF00;
  hid.usage = 1;
  extended.hid_bindings.push_back(hid);
  hid.id = "kept";
  hid.extra_fields = extras;
  hid.device_extra_fields = extras;
  hid.control_extra_fields = extras;
  extended.hid_bindings.push_back(hid);
  const InputToken token{InputDevice::generic_hid, 2, 0};
  InputBinding hid_binding;
  hid_binding.cooldown_id = "other";
  hid_binding.chord.tokens = {token};
  hid_binding.extra_fields = extras;
  hid_binding.token_extra_fields.emplace(token, extras);
  extended.bindings.push_back(hid_binding);
  expect(!validate_profile(extended), "opaque fields do not weaken known-field validation");
  const auto encoded = serialize_profile_json(extended);
  expect(encoded.find("-0") != std::string::npos,
         "opaque negative zero retains its original numeric spelling");
  const auto negative_zero = json::parse("-0");
  const auto *integer_zero = std::get_if<json::Value>(&negative_zero);
  expect(integer_zero && integer_zero->is_integer() && integer_zero->is_number() &&
             integer_zero->as_integer() == 0 && integer_zero->as_number() == 0.0,
         "preserved negative zero retains known-field integer reader behavior");
  const auto decoded = parse_profile_json(encoded);
  const auto *restored = std::get_if<GameProfile>(&decoded);
  expect(restored && *restored == extended,
         "unknown root, entity, nested HID, bar, module and token fields round-trip exactly");
  expect(restored && serialize_profile_json(*restored) == encoded,
         "opaque JSON including full-width integers has byte-stable canonical output");

  // Field declarations are the one inventory: both the reader/serializer keys
  // and the extension classification derive from them, so these checks derive
  // their expectations from the same declaration instead of a key list.
  {
    namespace detail = layerc::core::detail;
    const auto parsed_encoded = json::parse(encoded);
    const auto *encoded_root = std::get_if<json::Value>(&parsed_encoded);
    if (!expect(encoded_root != nullptr,
                "the extended fixture serializes to parseable JSON")) return;
    const auto first_element = [](const json::Value &parent,
                                  const std::string_view key)
        -> const json::Value * {
      const auto *array = parent.find(key);
      if (array == nullptr || !array->is_array() || array->as_array().empty())
        return nullptr;
      return &array->as_array().front();
    };
    const auto check_object = [](const json::Value *object,
                                 const detail::ProfileObject kind,
                                 const std::string &path) {
      if (!expect(object != nullptr && object->is_object(),
                  path + " is a serialized object")) return;
      const auto known = detail::known_profile_fields(kind);
      for (const auto &[key, value] : object->as_object()) {
        // The fixture's only intentional opaque field.
        if (key == "future") continue;
        expect(std::ranges::find(known, key) != known.end(),
               path + "." + key + " is declared by its object");
      }
    };
    const auto *game_object = encoded_root->find("game");
    const auto *module_object = first_element(*encoded_root, "modules");
    const auto *cooldown_object = first_element(*encoded_root, "cooldowns");
    const auto *group_object = first_element(*encoded_root, "cooldownGroups");
    const auto *hid_object = first_element(*encoded_root, "hidBindings");
    const auto *device_object = hid_object == nullptr
                                    ? nullptr
                                    : hid_object->find("device");
    const auto *control_object = hid_object == nullptr
                                     ? nullptr
                                     : hid_object->find("control");
    const auto *panel_object = first_element(*encoded_root, "panels");
    const auto *bar_object = panel_object == nullptr
                                 ? nullptr
                                 : panel_object->find("bar");
    const auto *binding_object = first_element(*encoded_root, "bindings");
    const auto *token_object =
        binding_object == nullptr ? nullptr : first_element(*binding_object, "chord");
    check_object(encoded_root, detail::ProfileObject::root, "$");
    check_object(game_object, detail::ProfileObject::game, "$.game");
    check_object(module_object, detail::ProfileObject::module, "$.modules[0]");
    check_object(cooldown_object, detail::ProfileObject::cooldown, "$.cooldowns[0]");
    check_object(group_object, detail::ProfileObject::group, "$.cooldownGroups[0]");
    check_object(hid_object, detail::ProfileObject::hid, "$.hidBindings[0]");
    check_object(device_object, detail::ProfileObject::device,
                 "$.hidBindings[0].device");
    check_object(control_object, detail::ProfileObject::control,
                 "$.hidBindings[0].control");
    check_object(panel_object, detail::ProfileObject::panel, "$.panels[0]");
    check_object(bar_object, detail::ProfileObject::bar, "$.panels[0].bar");
    check_object(binding_object, detail::ProfileObject::binding, "$.bindings[0]");
    check_object(token_object, detail::ProfileObject::token, "$.bindings[0].chord[0]");

    const std::array kinds{
        std::pair{detail::ProfileObject::root, std::string_view{"root"}},
        std::pair{detail::ProfileObject::game, std::string_view{"game"}},
        std::pair{detail::ProfileObject::module, std::string_view{"module"}},
        std::pair{detail::ProfileObject::cooldown, std::string_view{"cooldown"}},
        std::pair{detail::ProfileObject::group, std::string_view{"group"}},
        std::pair{detail::ProfileObject::hid, std::string_view{"hid"}},
        std::pair{detail::ProfileObject::device, std::string_view{"device"}},
        std::pair{detail::ProfileObject::control, std::string_view{"control"}},
        std::pair{detail::ProfileObject::panel, std::string_view{"panel"}},
        std::pair{detail::ProfileObject::bar, std::string_view{"bar"}},
        std::pair{detail::ProfileObject::binding, std::string_view{"binding"}},
        std::pair{detail::ProfileObject::token, std::string_view{"token"}},
    };
    const auto payload_value = json::Value(extras);
    const auto throws_invalid_argument = [](const auto &call) {
      try {
        call();
      } catch (const std::invalid_argument &) {
        return true;
      } catch (...) {
        return false;
      }
      return false;
    };
    for (const auto &[kind, kind_name] : kinds) {
      const auto known = detail::known_profile_fields(kind);
      if (!expect(!known.empty(), std::string(kind_name) + " declares fields")) continue;
      // A declared name is automatically classified: it never becomes opaque.
      json::Value::Object declared;
      for (const auto &name : known) declared.emplace(std::string(name), json::Value(nullptr));
      declared.emplace("futureUnrecognized", payload_value);
      const auto opaque =
          detail::unknown_profile_fields(json::Value(std::move(declared)), kind);
      expect(opaque.size() == 1 && opaque.contains("futureUnrecognized") &&
                 opaque.at("futureUnrecognized") == payload_value,
             "declared " + std::string(kind_name) +
                 " names are excluded from opaque capture automatically");
      // No declared name may be re-introduced as an extension, including the
      // legacy-only key and names a conditional serializer branch omits.
      for (const auto &name : known)
        expect(throws_invalid_argument([&] {
                 (void)detail::with_profile_extensions(
                     json::Value::Object{},
                     json::Value::Object{{std::string(name), json::Value(nullptr)}},
                     kind);
               }),
               "extension collision with declared " + std::string(kind_name) +
                   " field is refused");
    }
    // An emitted object field that someone forgot to declare must still be
    // refused, so a new serializer member cannot silently drop opaque data.
    expect(throws_invalid_argument([] {
             (void)detail::with_profile_extensions(
                 json::Value::Object{{"addedLater", json::Value(true)}},
                 json::Value::Object{{"addedLater", json::Value(false)}},
                 detail::ProfileObject::root);
           }),
           "an unregistered emitted field cannot overwrite an extension");

    // A recognized legacy field is migrated on parse, never emitted again.
    const auto legacy_serialized = serialize_profile_json(*source);
    expect(source->cooldowns.front().recovery == RecoveryMode::sequential &&
               !source->cooldowns.front().extra_fields.contains("stackCharges") &&
               legacy_serialized.find("stackCharges") == std::string::npos,
           "legacy stackCharges is migrated and never emitted");
    const auto legacy_cooldowns =
        layerc::core::profile_keys::cooldown::fields;
    std::size_t legacy_only{};
    bool stack_charges_declared{};
    for (const auto &field : legacy_cooldowns) {
      if (field.recognized_legacy_only) ++legacy_only;
      if (field.name == "stackCharges")
        stack_charges_declared = field.recognized_legacy_only;
    }
    expect(legacy_only == 1 && stack_charges_declared &&
               std::ranges::find(layerc::core::profile_keys::cooldown::names,
                                 std::string_view("stackCharges")) !=
                   layerc::core::profile_keys::cooldown::names.end(),
           "exactly the retired stackCharges declaration is legacy-only");
  }
  auto changed = extended;
  changed.extra_fields["future"] = json::Value(false);
  expect(changed != extended, "opaque edits participate in document value equality");
  compact_hid_bindings(extended);
  expect(extended.hid_bindings.size() == 1 &&
         extended.bindings.back().chord.tokens.front().code == 1 &&
         extended.bindings.back().token_extra_fields.at(
             extended.bindings.back().chord.tokens.front()) == extras,
         "HID compaction remaps token metadata by identity");
  extended.bindings.pop_back();
  compact_hid_bindings(extended);
  expect(extended.hid_bindings.size() == 1,
         "unreferenced HID descriptors carrying unknown fields are retained");
  auto invalid = extended;
  invalid.extra_fields["name"] = json::Value(std::string("shadow"));
  expect(validate_profile(invalid).has_value(), "known-field extension collisions are rejected");
  invalid = extended;
  invalid.panels.front().bar.extra_fields["bad"] =
      json::Value(std::numeric_limits<double>::infinity());
  expect(validate_profile(invalid).has_value(), "non-finite opaque data cannot be saved");
  invalid = extended;
  invalid.game.extra_fields["bad"] = json::Value(std::string(1, static_cast<char>(0xFF)));
  expect(validate_profile(invalid).has_value(), "invalid opaque UTF-8 is rejected, not discarded");
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

  // The reader reads any bindingId string; validate_profile alone rejects an
  // empty or unknown ID, with one diagnostic for both paths.
  auto empty_binding_id = std::string(source);
  constexpr std::string_view pedal_ref = R"json("bindingId":"pedal")json";
  if (const auto pedal = empty_binding_id.find(pedal_ref);
      pedal != std::string::npos)
    empty_binding_id.replace(pedal, pedal_ref.size(),
                             R"json("bindingId":"")json");
  const auto empty_id_result = parse_profile_json(empty_binding_id);
  const auto *empty_id_error = std::get_if<ProfileError>(&empty_id_result);
  if (profile != nullptr) {
    auto direct = *profile;
    direct.bindings.front().chord.tokens.front().code = 0;
    const auto expected = validate_profile(direct);
    expect(empty_id_error != nullptr && expected &&
               *empty_id_error == *expected &&
               empty_id_error->path == "$.bindings[0].chord[0].bindingId",
           "an empty HID bindingId fails once, in validate_profile");
  }
}

void test_panel_geometry() {
  using namespace layerc::core;
  Panel bar{.style = PanelStyle::bar};
  const auto grouped = panel_geometry(bar, 2);
  expect(grouped.size == PanelSize{272, 86} && grouped.items.size() == 2 &&
             grouped.items[0] == PanelRect{6, 6, 266, 40} &&
             grouped.items[1] == PanelRect{6, 46, 266, 80},
         "bar panels should share padding, spacing, and item geometry");
  const auto grouped_bar = bar_geometry(bar, 0, 2, 0.5);
  expect(grouped_bar.grouped &&
             grouped_bar.outline == PanelRect{6, 6, 266, 40} &&
             grouped_bar.fill == PanelRect{8, 8, 136, 38},
         "grouped bar fill should use the shared inset and progress track");
  const auto single_bar = bar_geometry(bar, 0, 1, 0.5);
  expect(!single_bar.grouped &&
             single_bar.outline == PanelRect{6, 6, 136, 40} &&
             single_bar.text == PanelRect{14, 8, 128, 38},
         "single bar shape and text hit area should share progress geometry");

  Panel cards{.layout = PanelLayout::grid, .style = PanelStyle::card};
  const auto grid = panel_geometry(cards, 5);
  expect(grid.size == PanelSize{258, 162} && grid.items.size() == 5 &&
             grid.items[4] == PanelRect{90, 84, 168, 156},
         "grid preview and overlays should use one square layout");
  cards.layout = PanelLayout::column;
  const auto column = panel_geometry(cards, 3);
  expect(column.size == PanelSize{90, 240} &&
             column.items[2] == PanelRect{6, 162, 84, 234},
         "column layout should place every card on one vertical axis");
}

// validate_profile is the one owner of profile semantics: an in-memory edit
// and the same edit read from JSON report the identical error.
void test_profile_validation_owns_semantics() {
  using namespace layerc::core;
  const auto base = std::get<GameProfile>(parse_profile_json(valid_profile));
  expect(!validate_profile(base), "a parsed profile must validate");

  struct Case {
    std::string_view name;
    void (*edit)(GameProfile &);
    std::string_view path;
  };
  const Case cases[] = {
      {"empty id", [](GameProfile &p) { p.id.clear(); }, "$.id"},
      {"no executable", [](GameProfile &p) { p.game = {}; }, "$.game"},
      {"duplicate cooldown",
       [](GameProfile &p) { p.cooldowns.push_back(p.cooldowns.front()); },
       "$.cooldowns[1].id"},
      {"zero duration",
       [](GameProfile &p) { p.cooldowns.front().duration = {}; },
       "$.cooldowns[0].durationMs"},
      {"too many charges",
       [](GameProfile &p) { p.cooldowns.front().max_charges = 100; },
       "$.cooldowns[0].maxCharges"},
      {"panel anchor", [](GameProfile &p) { p.panels.front().anchor_x = 2.0; },
       "$.panels[0].anchorX"},
      {"bar width", [](GameProfile &p) { p.panels.front().bar.width = 1; },
       "$.panels[0].bar.width"},
      {"unknown panel cooldown",
       [](GameProfile &p) { p.panels.front().cooldown_ids = {"ghost"}; },
       "$.panels[0].cooldowns[0]"},
      {"unknown binding cooldown",
       [](GameProfile &p) { p.bindings.front().cooldown_id = "ghost"; },
       "$.bindings[0].cooldownId"},
      {"mouse code",
       [](GameProfile &p) {
         p.bindings.front().chord.tokens.front() = {InputDevice::mouse, 8, 0};
       },
       "$.bindings[0].chord[0].code"},
      {"controller gap",
       [](GameProfile &p) {
         p.bindings.front().chord.tokens.front() = {InputDevice::controller,
                                                    11, 0};
       },
       "$.bindings[0].chord[0].code"},
      {"duplicate token",
       [](GameProfile &p) {
         auto &tokens = p.bindings.front().chord.tokens;
         tokens.push_back(tokens.front());
       },
       "$.bindings[0].chord[1]"},
      {"unknown HID binding",
       [](GameProfile &p) {
         p.bindings.front().chord.tokens.front() = {InputDevice::generic_hid,
                                                    1, 0};
       },
       "$.bindings[0].chord[0].bindingId"},
  };
  for (const auto &test : cases) {
    auto profile = base;
    test.edit(profile);
    const auto direct = validate_profile(profile);
    expect(direct && direct->path == test.path,
           std::string(test.name) + " must fail at " + std::string(test.path));
    if (test.name == "unknown HID binding") continue; // has no JSON spelling
    const auto parsed = parse_profile_json(serialize_profile_json(profile));
    const auto *error = std::get_if<ProfileError>(&parsed);
    expect(error != nullptr && direct && *error == *direct,
           std::string(test.name) + " must fail identically when parsed");
  }
}

} // namespace layerc::test
