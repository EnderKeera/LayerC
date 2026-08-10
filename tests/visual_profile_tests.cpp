#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/visual.hpp"
#include "layerc/core/calibration.hpp"

#include <string>
#include <variant>

namespace layerc::test {
namespace {

layerc::core::GameProfile visual_base_profile() {
  using namespace layerc::core;
  GameProfile profile;
  profile.id = "game";
  profile.name = "Game";
  profile.game.executable_name = "game.exe";
  profile.cooldowns.push_back(
      {.id = "dash", .name = "Dash", .duration = Milliseconds(1'000),
       .max_charges = 2, .stack_charges = true});
  profile.bindings.push_back(
      {.cooldown_id = "dash",
       .chord = InputChord{{InputToken{InputDevice::keyboard, 0x12, 0}}}});
  return profile;
}

} // namespace

void test_visual_profile_parser() {
  using namespace layerc::core;
  auto base = visual_base_profile();
  VisualProfile profile;
  profile.profile_id = base.id;
  profile.sample_rate_hz = 15;
  profile.quality = CaptureQuality::low;
  profile.rules.push_back({.id = "ready",
                           .name = "Ready color",
                           .region = {0.0, 0.0, 1.0, 1.0},
                           .predicate = AverageColorPredicate{0x20A040, 8},
                           .stable_samples = 2,
                           .repeat = PixelRepeatPolicy::edge});
  profile.rules.push_back({.id = "bright",
                           .name = "Bright",
                           .region = {0.2, 0.3, 0.4, 0.5},
                           .predicate = BrightnessPredicate{
                               180, BrightnessComparison::above},
                           .stable_samples = 4});
  profile.rules.push_back({.id = "coverage",
                           .name = "Coverage",
                           .region = {0.1, 0.1, 0.2, 0.2},
                           .predicate = ColorCoveragePredicate{0x112233, 12,
                                                               0.75}});
  profile.triggers.push_back(
      {.cooldown_id = "dash",
       .rule_id = "ready",
       .mode = VisualTriggerMode::confirm_input,
       .confirmation_timeout = Milliseconds(750),
       .unavailable_policy = CaptureUnavailablePolicy::trigger_from_input});

  const auto serialized = serialize_visual_profile_json(profile);
  const auto parsed = parse_visual_profile_json(serialized, base);
  expect(std::holds_alternative<VisualProfile>(parsed) &&
             std::get<VisualProfile>(parsed) == profile,
         "visual sidecars should round-trip all V1.1 predicate types");

  profile.triggers.front().mode = VisualTriggerMode::pixel_only;
  profile.triggers.front().keep_input = false;
  const auto pixel_round_trip = parse_visual_profile_json(
      serialize_visual_profile_json(profile), base);
  expect(std::holds_alternative<VisualProfile>(pixel_round_trip) &&
             std::get<VisualProfile>(pixel_round_trip)
                     .triggers.front()
                     .unavailable_policy ==
                 CaptureUnavailablePolicy::trigger_from_input,
         "pixel-only sidecars should preserve capture failure policy");

  auto invalid = serialized;
  invalid.insert(invalid.find("\"rules\""), "\"unknown\": 1,\n  ");
  expect(std::holds_alternative<VisualError>(
             parse_visual_profile_json(invalid, base)),
         "visual parser should reject unknown fields");
  auto inapplicable = serialized;
  inapplicable.insert(inapplicable.find("\"confirmationTimeoutMs\""),
                      "\"keepInput\": false,\n      ");
  expect(std::holds_alternative<VisualError>(
             parse_visual_profile_json(inapplicable, base)),
         "visual parser should reject fields that do not apply to a trigger mode");
  expect(std::holds_alternative<VisualError>(parse_visual_profile_json(
             std::string(max_visual_json_bytes + 1, ' '), base)),
         "visual parser should enforce its bounded sidecar size");

  auto global = base;
  global.game.global = true;
  expect(!validate_visual_profile(profile, global).message.empty(),
         "global profiles should reject visual assignments");
  profile.triggers.front().cooldown_id = "missing";
  expect(!validate_visual_profile(profile, base).message.empty(),
         "visual assignments should reference a real cooldown");

  profile.triggers.front().cooldown_id = "dash";
  profile.triggers.front().enabled = false;
  expect(!has_enabled_visual_assignments(profile),
         "a sidecar whose assignments are all disabled should not require capture");
  profile.triggers.front().enabled = true;
  expect(has_enabled_visual_assignments(profile),
         "an enabled visual assignment should require capture");
}

void test_calibration_protocol() {
  using namespace layerc::core;
  CalibrationRequest request{.target_window = 0xFEDCBA9876543210ULL,
                             .cooldown_id = "dash",
                             .profile_json = "{\"name\":\"Game\"}",
                             .visual_json = "{\"rules\":[]}"};
  const auto decoded =
      decode_calibration_request(encode_calibration_request(request));
  const auto *round_trip = std::get_if<CalibrationRequest>(&decoded);
  expect(round_trip != nullptr &&
             round_trip->target_window == request.target_window &&
             round_trip->cooldown_id == request.cooldown_id &&
             round_trip->profile_json == request.profile_json &&
             round_trip->visual_json == request.visual_json,
         "calibration requests should preserve bounded JSON and 64-bit HWNDs");

  CalibrationResponse response{.applied = true,
                               .visual_json = "{\"schemaVersion\":1}"};
  const auto decoded_response =
      decode_calibration_response(encode_calibration_response(response));
  expect(std::holds_alternative<CalibrationResponse>(decoded_response) &&
             std::get<CalibrationResponse>(decoded_response).visual_json ==
                 response.visual_json,
         "calibration Apply responses should round-trip their one-rule sidecar");
  expect(std::holds_alternative<CalibrationProtocolError>(
             decode_calibration_response(
                 R"({"protocolVersion":1,"applied":true,"visualJson":"","error":""})")),
         "an applied calibration response must contain a rule assignment");
}

} // namespace layerc::test
