#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/visual.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/calibration.hpp"
#include "layerc/core/calibration_readiness.hpp"

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
       .max_charges = 2, .recovery = RecoveryMode::sequential});
  profile.bindings.push_back(
      {.cooldown_id = "dash",
       .chord = InputChord{{InputToken{InputDevice::keyboard, 0x12, 0}}}});
  return profile;
}

} // namespace

void test_visual_profile_parser() {
  using namespace layerc::core;
  const NormalizedRegion full{0.0, 0.0, 1.0, 1.0};
  const NormalizedRegion inside{0.25, 0.25, 0.5, 0.5};
  const NormalizedRegion disjoint{0.75, 0.0, 0.25, 0.25};
  expect(contains(full, inside) && !contains(inside, full) &&
             !overlaps(inside, disjoint),
         "shared normalized geometry distinguishes containment from contact");
  const NormalizedRegion almost_inside{-region_geometry_epsilon / 2, 0.0,
                                        0.5, 0.5};
  expect(contains(full, almost_inside) &&
             overlaps(inside, {0.75 - 2 * region_geometry_epsilon, 0.25,
                                0.25, 0.5}),
         "shared geometry applies one tolerance at both boundaries");
  const auto [left, top, right, bottom] =
      to_pixel_bounds({0.15, 0.25, 0.5, 0.5}, 10, 8);
  expect(left == 1 && top == 2 && right == 7 && bottom == 6,
         "shared pixel bounds floor low edges and ceil high edges");
  auto base = visual_base_profile();
  VisualProfile profile;
  profile.profile_id = base.id;
  profile.sample_rate_hz = 15;
  profile.quality = CaptureQuality::low;
  profile.observations.push_back(layerc::core::PixelRule{.id = "ready",
                           .name = "Ready color",
                           .region = {0.0, 0.0, 1.0, 1.0},
                           .predicate = AverageColorPredicate{0x20A040, 8},
                           .stable_samples = 2,
                           .repeat = PixelRepeatPolicy::edge});
  profile.observations.push_back(layerc::core::PixelRule{.id = "bright",
                           .name = "Bright",
                           .region = {0.2, 0.3, 0.4, 0.5},
                           .predicate = BrightnessPredicate{
                               180, BrightnessComparison::above},
                           .stable_samples = 4});
  profile.observations.push_back(layerc::core::PixelRule{.id = "coverage",
                           .name = "Coverage",
                           .region = {0.1, 0.1, 0.2, 0.2},
                           .predicate = ColorCoveragePredicate{0x112233, 12,
                                                               0.75}});
  profile.triggers.push_back(
      {.cooldown_id = "dash",
       .observation_id = "ready",
       .mode = VisualTriggerMode::confirm_input,
       .confirmation_timeout = Milliseconds(750),
       .unavailable_policy = CaptureUnavailablePolicy::trigger_from_input});

  const auto serialized = serialize_visual_profile_json(profile);
  const auto syntax_error = parse_visual_profile_json("{]", base);
  expect(std::holds_alternative<VisualError>(syntax_error) &&
             std::get<VisualError>(syntax_error).path == "$@1",
         "visual syntax errors retain their byte offset in the path");
  VisualProfile empty;
  empty.profile_id = "game";
  expect(serialize_visual_profile_json(empty) ==
             "{\r\n  \"observations\": [],\r\n  \"profileId\": \"game\",\r\n"
             "  \"quality\": \"balanced\",\r\n  \"sampleRateHz\": 10,\r\n"
             "  \"schemaVersion\": 2,\r\n  \"triggers\": []\r\n}\r\n",
         "visual sidecar empty-array layout is byte-stable");
  const auto parsed = parse_visual_profile_json(serialized, base);
  expect(std::holds_alternative<VisualProfile>(parsed) &&
             std::get<VisualProfile>(parsed) == profile,
         "visual sidecars should round-trip pixel observations through schema v2");
  expect(serialized.find("\"schemaVersion\": 2") != std::string::npos &&
             serialized.find("\"type\": \"pixel\"") != std::string::npos,
         "saving a visual profile should emit typed schema-v2 observations");

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
  invalid.insert(invalid.find("\"observations\""), "\"unknown\": 1,\n  ");
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

  VisualProfile template_profile;
  template_profile.profile_id = base.id;
  template_profile.observations.push_back(
      layerc::core::TemplateState{.id = "ready-template",
       .name = "Ready template",
       .search_region = {0.1, 0.2, 0.6, 0.5},
       .expected_width = 0.2,
       .expected_height = 0.1,
       .references = {{std::string(64, 'a')}, {std::string(64, 'b')}},
       .confidence_threshold = 0.91,
       .scale_tolerance = 0.1,
       .color_tolerance = 30,
       .stable_samples = 3,
       .repeat = PixelRepeatPolicy::when_fully_ready});
  template_profile.triggers.push_back(
      {.cooldown_id = "dash",
       .observation_id = "ready-template",
       .mode = VisualTriggerMode::pixel_only});
  const auto template_json =
      serialize_visual_profile_json(template_profile);
  const auto template_round_trip =
      parse_visual_profile_json(template_json, base);
  expect(std::holds_alternative<VisualProfile>(template_round_trip) &&
             std::get<VisualProfile>(template_round_trip) ==
                 template_profile,
         "template observations should round-trip all bounded matching settings");

  // An authored [template, pixel] order survives save and reload, and an
  // observation's index is its path (layer45 F-V1).
  VisualProfile mixed = template_profile;
  mixed.observations.push_back(profile.observations.front());
  const auto mixed_json = serialize_visual_profile_json(mixed);
  const auto mixed_round_trip = parse_visual_profile_json(mixed_json, base);
  const auto *mixed_parsed = std::get_if<VisualProfile>(&mixed_round_trip);
  expect(mixed_parsed != nullptr && *mixed_parsed == mixed &&
             mixed_parsed->observations.size() == 2 &&
             std::holds_alternative<TemplateState>(
                 mixed_parsed->observations[0]) &&
             std::holds_alternative<PixelRule>(mixed_parsed->observations[1]) &&
             serialize_visual_profile_json(*mixed_parsed) == mixed_json &&
             mixed_json.find("\"template\"") < mixed_json.find("\"pixel\""),
         "authored observation order round-trips through save unchanged");
  std::get<TemplateState>(mixed.observations.front())
      .references.front()
      .asset_sha256 = std::string(64, 'A');
  const auto mixed_error = parse_visual_profile_json(
      serialize_visual_profile_json(mixed), base);
  expect(std::holds_alternative<VisualError>(mixed_error) &&
             std::get<VisualError>(mixed_error).path ==
                 "$.observations[0].references[0].asset",
         "semantic template errors cite the authored observation index");
  expect(validate_visual_profile(mixed, base).path ==
             "$.observations[0].references[0].asset",
         "in-memory validation cites the same authored observation index");
  VisualProfile duplicate = template_profile;
  duplicate.observations.push_back(profile.observations.front());
  std::get<TemplateState>(duplicate.observations.front()).id =
      observation_id(duplicate.observations.back());
  const auto duplicate_error = validate_visual_profile(duplicate, base);
  expect(duplicate_error.path == "$.observations[1].id" &&
             duplicate_error.message == "duplicate visual observation id",
         "validator errors cite schema-2 observation paths without a mapper");
  VisualProfile crowded = template_profile;
  crowded.observations.resize(max_template_states + 1,
                              template_profile.observations.front());
  expect(validate_visual_profile(crowded, base).path == "$.observations",
         "observation count errors cite the schema-2 observations array");
  std::get<TemplateState>(template_profile.observations.front())
      .references.front()
      .asset_sha256 =
       std::string(64, 'A');
  expect(!validate_visual_profile(template_profile, base).message.empty(),
         "template references should reject noncanonical asset hashes");

  constexpr std::string_view legacy_visual = R"json({
    "schemaVersion":1,"profileId":"game","sampleRateHz":10,
    "quality":"balanced","rules":[{
      "id":"legacy","name":"Legacy","region":{"x":0,"y":0,"width":1,"height":1},
      "predicate":{"type":"brightness","comparison":"above","threshold":100},
      "stableSamples":2,"repeat":"edge"}],
    "triggers":[{"cooldownId":"dash","ruleId":"legacy","enabled":true,
      "mode":"pixelOnly","keepInput":false,"onUnavailable":"ignore"}]
  })json";
  const auto migrated = parse_visual_profile_json(legacy_visual, base);
  expect(std::holds_alternative<VisualProfile>(migrated) &&
             std::get<VisualProfile>(migrated).schema_version == 2 &&
             serialize_visual_profile_json(
                 std::get<VisualProfile>(migrated))
                     .find("\"observationId\"") != std::string::npos,
         "schema-v1 visual sidecars should migrate in memory and save as v2");
}

// The visual reader reads shape and types only; validate_visual_profile is the
// sole owner of semantic rules, so a parsed sidecar and the same profile built
// in memory report the identical path and message (layer45 F-C1/F-V2).
void test_visual_profile_parse_validate_parity() {
  using namespace layerc::core;
  const auto base = visual_base_profile();
  VisualProfile valid;
  valid.profile_id = base.id;
  valid.observations.push_back(TemplateState{
      .id = "ready-template",
      .name = "Ready template",
      .search_region = {0.1, 0.2, 0.6, 0.5},
      .expected_width = 0.2,
      .expected_height = 0.1,
      .references = {{std::string(64, 'a')}},
      .confidence_threshold = 0.9,
      .scale_tolerance = 0.1,
      .color_tolerance = 30,
      .stable_samples = 3});
  valid.observations.push_back(PixelRule{
      .id = "ready",
      .name = "Ready",
      .region = {0.0, 0.0, 1.0, 1.0},
      .predicate = AverageColorPredicate{0x20A040, 8},
      .stable_samples = 2});
  valid.triggers.push_back({.cooldown_id = "dash",
                            .observation_id = "ready",
                            .mode = VisualTriggerMode::pixel_only});
  expect(validate_visual_profile(valid, base).message.empty() &&
             std::holds_alternative<VisualProfile>(parse_visual_profile_json(
                 serialize_visual_profile_json(valid), base)),
         "the parity fixture is a valid visual sidecar");

  struct Case {
    std::string_view name;
    void (*edit)(VisualProfile &);
    std::string_view path;
  };
  static constexpr auto pixel_rule = [](VisualProfile &p) -> PixelRule & {
    return std::get<PixelRule>(p.observations[1]);
  };
  static constexpr auto template_state =
      [](VisualProfile &p) -> TemplateState & {
    return std::get<TemplateState>(p.observations[0]);
  };
  const Case cases[] = {
      {"empty profile id", [](VisualProfile &p) { p.profile_id.clear(); },
       "$.profileId"},
      {"empty pixel id", [](VisualProfile &p) { pixel_rule(p).id.clear(); },
       "$.observations[1]"},
      {"empty pixel name", [](VisualProfile &p) { pixel_rule(p).name.clear(); },
       "$.observations[1]"},
      {"empty template id",
       [](VisualProfile &p) { template_state(p).id.clear(); },
       "$.observations[0]"},
      {"empty template name",
       [](VisualProfile &p) { template_state(p).name.clear(); },
       "$.observations[0]"},
      {"empty template asset",
       [](VisualProfile &p) {
         template_state(p).references.front().asset_sha256.clear();
       },
       "$.observations[0].references[0].asset"},
      {"empty trigger cooldown",
       [](VisualProfile &p) { p.triggers.front().cooldown_id.clear(); },
       "$.triggers[0].cooldownId"},
      {"empty trigger observation",
       [](VisualProfile &p) { p.triggers.front().observation_id.clear(); },
       "$.triggers[0].observationId"},
  };
  for (const auto &test : cases) {
    auto profile = valid;
    test.edit(profile);
    const auto direct = validate_visual_profile(profile, base);
    expect(direct.path == test.path && !direct.message.empty(),
           std::string(test.name) + " must fail validation at " +
               std::string(test.path));
    const auto parsed = parse_visual_profile_json(
        serialize_visual_profile_json(profile), base);
    const auto *error = std::get_if<VisualError>(&parsed);
    expect(error != nullptr && error->path == direct.path &&
               error->message == direct.message,
           std::string(test.name) + " must fail identically when parsed");
  }

  // Schema 1 reaches the same validator; only the array name is remapped.
  constexpr std::string_view legacy_empty_id = R"json({
    "schemaVersion":1,"profileId":"game","sampleRateHz":10,
    "quality":"balanced","rules":[{
      "id":"","name":"Legacy","region":{"x":0,"y":0,"width":1,"height":1},
      "predicate":{"type":"brightness","comparison":"above","threshold":100},
      "stableSamples":2,"repeat":"edge"}],
    "triggers":[]
  })json";
  VisualProfile legacy;
  legacy.profile_id = base.id;
  legacy.observations.push_back(PixelRule{
      .name = "Legacy",
      .region = {0.0, 0.0, 1.0, 1.0},
      .predicate = BrightnessPredicate{100, BrightnessComparison::above},
      .stable_samples = 2});
  const auto legacy_direct = validate_visual_profile(legacy, base);
  const auto legacy_parsed = parse_visual_profile_json(legacy_empty_id, base);
  const auto *legacy_error = std::get_if<VisualError>(&legacy_parsed);
  expect(legacy_error != nullptr && legacy_error->path == "$.rules[0]" &&
             legacy_direct.path == "$.observations[0]" &&
             legacy_error->message == legacy_direct.message,
         "an empty schema-1 rule id fails once, in the validator");
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

  std::string escaped_document;
  json::append_value(escaped_document, json::Value(json::Value::Object{
      {"payload", json::Value(std::string(100U * 1024U, '\\'))}}));
  CalibrationRequest escaped_request{
      .target_window = 1,
      .cooldown_id = "dash",
      .profile_json = escaped_document,
      .visual_json = escaped_document};
  const auto escaped_packet = encode_calibration_request(escaped_request);
  const auto escaped_round_trip =
      decode_calibration_request(escaped_packet);
  expect(escaped_packet.size() < 512U * 1024U &&
             std::holds_alternative<CalibrationRequest>(
                 escaped_round_trip) &&
             std::get<CalibrationRequest>(escaped_round_trip).profile_json ==
                 escaped_request.profile_json &&
             std::get<CalibrationRequest>(escaped_round_trip).visual_json ==
                 escaped_request.visual_json,
         "nested calibration documents must avoid a second layer of escaping");

  CalibrationResponse response{.applied = true,
                               .visual_json = "{\"schemaVersion\":1}"};
  const auto decoded_response =
      decode_calibration_response(encode_calibration_response(response));
  expect(std::holds_alternative<CalibrationResponse>(decoded_response) &&
             std::get<CalibrationResponse>(decoded_response).visual_json ==
                 response.visual_json,
         "calibration Apply responses should round-trip their single-observation sidecar");
  expect(std::holds_alternative<CalibrationProtocolError>(
             decode_calibration_response(
                 R"({"protocolVersion":2,"applied":true,"visual":null,"error":""})")),
         "an applied calibration response must contain a rule assignment");
  const auto old_peer = decode_calibration_request(
      R"({"protocolVersion":1,"targetWindow":"1","cooldownId":"dash","profileJson":"{}"})");
  expect(std::holds_alternative<CalibrationProtocolError>(old_peer) &&
             !std::get<CalibrationProtocolError>(old_peer).message.empty(),
         "mismatched calibration peers must be rejected visibly");
  auto absent = request;
  absent.visual_json.clear();
  const auto absent_round_trip = decode_calibration_request(
      encode_calibration_request(absent));
  expect(std::holds_alternative<CalibrationRequest>(absent_round_trip) &&
             std::get<CalibrationRequest>(absent_round_trip).visual_json.empty(),
         "an absent calibration sidecar must round-trip as JSON null");
  for (const auto packet : {
           R"({"protocolVersion":2,"targetWindow":"","cooldownId":"dash","profile":{},"visual":null})",
           R"({"protocolVersion":2,"targetWindow":"1","cooldownId":"","profile":{},"visual":null})",
           R"({"protocolVersion":2,"targetWindow":"1","cooldownId":"dash","profile":"{}","visual":null})",
           R"({"protocolVersion":2,"targetWindow":"1","cooldownId":"dash","profile":{},"visual":[]})"}) {
    const auto invalid = decode_calibration_request(packet);
    const auto *error = std::get_if<CalibrationProtocolError>(&invalid);
    expect(error != nullptr && !error->message.empty() &&
               error->message.starts_with("$."),
           "empty required calibration fields must retain a useful field diagnostic");
  }
}

void test_calibration_readiness() {
  using namespace layerc::core;
  CalibrationReadiness readiness;
  PixelRule rule;
  rule.id = "sample";
  rule.name = "Sampling readiness";
  rule.region = {0.1, 0.1, 0.2, 0.2};
  rule.predicate = AverageColorPredicate{0xFFFFFFU, 24};
  constexpr auto quality = CaptureQuality::balanced;
  expect(!readiness.ready_for(VisualObservation{rule}, quality),
         "valid defaults must not imply sampled calibration");
  PixelEvaluation pixels;
  readiness.observe(rule, pixels, quality, true);
  expect(!readiness.ready_for(VisualObservation{rule}, quality),
         "zero sampled pixels must not enable Apply");
  pixels.sampled_pixels = 1;
  pixels.matched = false;
  readiness.observe(rule, pixels, quality, false);
  expect(!readiness.ready_for(VisualObservation{rule}, quality),
         "unavailable pixels must not enable Apply");
  readiness.observe(rule, pixels, quality, true);
  expect(readiness.ready_for(VisualObservation{rule}, quality),
         "a genuine no-match still evaluates the predicate");
  expect(!readiness.ready_for(VisualObservation{rule}, CaptureQuality::low),
         "quality changes require evaluation at that quality");
  auto changed = rule;
  changed.region.x += 0.1;
  expect(!readiness.ready_for(VisualObservation{changed}, quality),
         "changed regions must not reuse old evidence");
  changed = rule;
  changed.predicate = AverageColorPredicate{0U, 24};
  expect(!readiness.ready_for(VisualObservation{changed}, quality),
         "changed target colours must not reuse old evidence");
  changed = rule;
  changed.stable_samples += 1;
  expect(!readiness.ready_for(VisualObservation{changed}, quality),
         "changed predicate settings must not reuse old evidence");
  TemplateState state;
  state.id = "template";
  state.name = "Template readiness";
  TemplateMatchResult result;
  result.search_region_available = true;
  result.candidates_evaluated = 1;
  readiness.observe(state, result, quality);
  expect(!readiness.ready_for(VisualObservation{state}, quality),
         "examining but not scoring a candidate provides no evidence");
  result.candidates_scored = 1;
  result.matched = false;
  result.search_region_available = false;
  readiness.observe(state, result, quality);
  expect(!readiness.ready_for(VisualObservation{state}, quality),
         "unavailable template regions provide no evidence");
  result.search_region_available = true;
  readiness.observe(state, result, quality);
  expect(readiness.ready_for(VisualObservation{state}, quality),
         "a scored negative template candidate is valid evaluation evidence");
  auto changed_state = state;
  changed_state.search_region.x += 0.1;
  expect(!readiness.ready_for(VisualObservation{changed_state}, quality),
         "changed template search geometry requires reevaluation");
  changed_state = state;
  changed_state.references.push_back({.asset_sha256 = std::string(64, 'a')});
  expect(!readiness.ready_for(VisualObservation{changed_state}, quality),
         "loading/importing or modifying a reference must not seed evidence");
  expect(!readiness.ready_for(VisualObservation{rule}, quality),
         "switching predicate modes must not reuse a different observation");
  readiness.invalidate();
  expect(!readiness.ready_for(VisualObservation{state}, quality),
         "clearing capture evidence disables Apply");
}

} // namespace layerc::test
