#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/pixel_engine.hpp"

#include <chrono>
#include <vector>

namespace layerc::test {
namespace {

layerc::core::VisualProfile visual_profile(
    const layerc::core::VisualTriggerMode mode =
        layerc::core::VisualTriggerMode::pixel_only,
    const layerc::core::PixelRepeatPolicy repeat =
        layerc::core::PixelRepeatPolicy::edge) {
  using namespace layerc::core;
  VisualProfile profile;
  profile.profile_id = "game";
  profile.sample_rate_hz = 10;
  profile.rules.push_back({.id = "ready",
                           .name = "Ready",
                           .region = {0.0, 0.0, 1.0, 1.0},
                           .predicate = AverageColorPredicate{0x20A040, 8},
                           .stable_samples = 2,
                           .repeat = repeat});
  profile.triggers.push_back({.cooldown_id = "dash",
                              .rule_id = "ready",
                              .enabled = true,
                              .mode = mode,
                              .keep_input = false,
                              .confirmation_timeout = Milliseconds(500),
                              .unavailable_policy =
                                  CaptureUnavailablePolicy::ignore});
  return profile;
}

layerc::core::PixelRuleMeasurement measurement(
    const bool matched, const layerc::core::TimePoint at,
    const layerc::core::CaptureAvailability availability =
        layerc::core::CaptureAvailability::available) {
  return {.rule_id = "ready",
          .availability = availability,
          .matched = matched,
          .sampled_at = at};
}

} // namespace

void test_pixel_rule_evaluation() {
  using namespace layerc::core;
  std::vector<RgbPixel> pixels(100U * 100U, {20, 40, 60});
  PixelFrameView frame{pixels, 100, 100, 0};
  PixelRule average{.id = "average",
                    .name = "Average",
                    .region = {0.0, 0.0, 1.0, 1.0},
                    .predicate = AverageColorPredicate{0x14283C, 0}};
  const auto exact = evaluate_pixel_rule(average, frame, CaptureQuality::exact);
  const auto low = evaluate_pixel_rule(average, frame, CaptureQuality::low);
  expect(exact.matched && exact.average_color == 0x14283C &&
             exact.sampled_pixels == 10'000,
         "exact evaluation should inspect every region pixel");
  expect(low.matched && low.sampled_pixels == 64U * 64U,
         "low evaluation should use the bounded deterministic grid");

  for (std::size_t index = 0; index < pixels.size() / 2; ++index)
    pixels[index] = {200, 10, 10};
  PixelRule coverage{.id = "coverage",
                     .name = "Coverage",
                     .region = {0.0, 0.0, 1.0, 1.0},
                     .predicate = ColorCoveragePredicate{0xC80A0A, 0, 0.5}};
  const auto coverage_result =
      evaluate_pixel_rule(coverage, frame, CaptureQuality::exact);
  expect(coverage_result.matched && coverage_result.color_coverage == 0.5,
         "coverage evaluation should report the matching pixel fraction");

  PixelRule brightness{.id = "brightness",
                       .name = "Brightness",
                       .region = {0.0, 0.0, 1.0, 1.0},
                       .predicate = BrightnessPredicate{
                           10, BrightnessComparison::above}};
  expect(evaluate_pixel_rule(brightness, frame, CaptureQuality::balanced)
             .matched,
         "brightness predicates should use the sampled average luminance");
}

void test_visual_trigger_state_machine() {
  using namespace std::chrono_literals;
  using namespace layerc::core;
  const TimePoint start{};
  auto profile = visual_profile();
  VisualTriggerEngine engine(profile);
  const std::vector<CooldownState> ready{
      {.id = "dash", .available_charges = 2, .max_charges = 2}};

  expect(engine.on_input("dash", start).empty(),
         "pixel-only replace mode should suppress its base binding");
  expect(engine.on_measurements(
                    std::vector{measurement(true, start)}, ready, start)
             .empty(),
         "a rule should wait for its configured stable sample count");
  auto fired = engine.on_measurements(
      std::vector{measurement(true, start + 100ms)}, ready, start + 100ms);
  expect(fired == std::vector<VisualTriggerRequest>{{
                      "dash", VisualTriggerSource::pixel_edge}},
         "a stable false-to-true edge should trigger once");
  expect(engine.on_measurements(
                    std::vector{measurement(true, start + 200ms)}, ready,
                    start + 200ms)
             .empty(),
         "a held pixel edge should not repeat");
  (void)engine.on_measurements(
      std::vector{measurement(false, start + 300ms)}, ready, start + 300ms);
  (void)engine.on_measurements(
      std::vector{measurement(false, start + 400ms)}, ready, start + 400ms);
  (void)engine.on_measurements(
      std::vector{measurement(true, start + 500ms)}, ready, start + 500ms);
  expect(engine.on_measurements(
                    std::vector{measurement(true, start + 600ms)}, ready,
                    start + 600ms)
                 .size() == 1,
         "a stable false interval should rearm the pixel edge");

  profile = visual_profile(VisualTriggerMode::confirm_input);
  VisualTriggerEngine confirm(profile);
  (void)confirm.on_measurements(
      std::vector{measurement(true, start)}, ready, start);
  (void)confirm.on_measurements(
      std::vector{measurement(true, start + 100ms)}, ready, start + 100ms);
  expect(confirm.on_input("dash", start + 150ms) ==
             std::vector<VisualTriggerRequest>{{
                 "dash", VisualTriggerSource::input_confirmed}},
         "input-first confirmation should immediately use a fresh stable match");

  VisualTriggerEngine recovered_confirmation(profile);
  (void)recovered_confirmation.on_measurements(
      std::vector{measurement(true, start)}, ready, start);
  (void)recovered_confirmation.on_measurements(
      std::vector{measurement(true, start + 100ms)}, ready, start + 100ms);
  (void)recovered_confirmation.on_measurements(
      std::vector{measurement(false, start + 200ms,
                              CaptureAvailability::unavailable)},
      ready, start + 200ms);
  (void)recovered_confirmation.on_measurements(
      std::vector{measurement(false, start + 300ms)}, ready, start + 300ms);
  expect(recovered_confirmation.on_input("dash", start + 301ms).empty(),
         "capture loss must clear a previously stable match before input confirmation");
  (void)confirm.on_measurements(
      std::vector{measurement(false, start + 200ms)}, ready, start + 200ms);
  (void)confirm.on_measurements(
      std::vector{measurement(false, start + 300ms)}, ready, start + 300ms);
  expect(confirm.on_input("dash", start + 350ms).empty(),
         "input confirmation should arm while the predicate is false");
  (void)confirm.on_measurements(
      std::vector{measurement(true, start + 400ms)}, ready, start + 400ms);
  expect(confirm.on_measurements(
             std::vector{measurement(true, start + 500ms)}, ready,
             start + 500ms) ==
             std::vector<VisualTriggerRequest>{{
                 "dash", VisualTriggerSource::input_confirmed}},
         "a predicate that stabilizes inside the input window should confirm");

  profile.triggers.front().unavailable_policy =
      CaptureUnavailablePolicy::trigger_from_input;
  VisualTriggerEngine fallback(profile);
  (void)fallback.on_measurements(
      std::vector{measurement(false, start, CaptureAvailability::unavailable)},
      ready, start);
  expect(fallback.on_input("dash", start + 1ms) ==
             std::vector<VisualTriggerRequest>{{
                 "dash", VisualTriggerSource::capture_fallback}},
         "a confirmed assignment may explicitly fall back on capture failure");
  fallback.set_visual_enabled(false);
  expect(fallback.on_input("dash", start + 2ms) ==
             std::vector<VisualTriggerRequest>{{
                 "dash", VisualTriggerSource::direct_input}},
         "disabling visual triggers should restore ordinary input routing");

  profile = visual_profile(VisualTriggerMode::pixel_only);
  profile.triggers.front().unavailable_policy =
      CaptureUnavailablePolicy::trigger_from_input;
  VisualTriggerEngine pixel_fallback(profile);
  (void)pixel_fallback.on_measurements(
      std::vector{measurement(false, start,
                              CaptureAvailability::protected_or_black)},
      ready, start);
  expect(pixel_fallback.on_input("dash", start + 3ms) ==
             std::vector<VisualTriggerRequest>{{
                 "dash", VisualTriggerSource::capture_fallback}},
         "pixel-only assignments should honor explicit input fallback when capture is unavailable");

  VisualTriggerEngine starting_pixel_fallback(profile);
  expect(starting_pixel_fallback.on_input("dash", start + 4ms) ==
             std::vector<VisualTriggerRequest>{{
                 "dash", VisualTriggerSource::capture_fallback}},
         "pixel-only input fallback should work before the first capture sample");
  (void)starting_pixel_fallback.on_measurements(
      std::vector{measurement(false, start + 5ms,
                              CaptureAvailability::starting)},
      ready, start + 5ms);
  expect(starting_pixel_fallback.on_input("dash", start + 6ms) ==
             std::vector<VisualTriggerRequest>{{
                 "dash", VisualTriggerSource::capture_fallback}},
         "pixel-only input fallback should work while capture is starting");

  profile = visual_profile(VisualTriggerMode::confirm_input);
  profile.triggers.front().unavailable_policy =
      CaptureUnavailablePolicy::trigger_from_input;
  VisualTriggerEngine starting_confirmation_fallback(profile);
  expect(starting_confirmation_fallback.on_input("dash", start + 7ms) ==
             std::vector<VisualTriggerRequest>{{
                 "dash", VisualTriggerSource::capture_fallback}},
         "confirmation input fallback should work before the first capture sample");
  (void)starting_confirmation_fallback.on_measurements(
      std::vector{measurement(false, start + 8ms,
                              CaptureAvailability::starting)},
      ready, start + 8ms);
  expect(starting_confirmation_fallback.on_input("dash", start + 9ms) ==
             std::vector<VisualTriggerRequest>{{
                 "dash", VisualTriggerSource::capture_fallback}},
         "confirmation input fallback should work while capture is starting");
}

void test_visual_repeat_when_ready() {
  using namespace std::chrono_literals;
  using namespace layerc::core;
  const TimePoint start{};
  VisualTriggerEngine engine(visual_profile(
      VisualTriggerMode::pixel_only, PixelRepeatPolicy::when_fully_ready));
  const std::vector<CooldownState> ready{
      {.id = "dash", .available_charges = 2, .max_charges = 2}};
  const std::vector<CooldownState> cooling{
      {.id = "dash",
       .available_charges = 1,
       .max_charges = 2,
       .next_charge_at = start + 1s}};
  (void)engine.on_measurements(
      std::vector{measurement(true, start)}, ready, start);
  expect(engine.on_measurements(
             std::vector{measurement(true, start + 100ms)}, ready,
             start + 100ms) ==
             std::vector<VisualTriggerRequest>{{
                 "dash", VisualTriggerSource::pixel_repeat}},
         "repeat mode should fire when a stable rule is fully ready");
  engine.on_cooldown_triggered("dash");
  expect(engine.on_measurements(
             std::vector{measurement(true, start + 200ms)}, ready,
             start + 200ms) ==
             std::vector<VisualTriggerRequest>{{
                 "dash", VisualTriggerSource::pixel_repeat}},
         "repeat mode should rearm when an accepted cooldown completes between visual samples");
  expect(engine.on_measurements(
                    std::vector{measurement(true, start + 225ms)}, ready,
                    start + 225ms)
             .empty(),
         "repeat mode should latch a single fully-ready interval");
  (void)engine.on_measurements(
      std::vector{measurement(false, start + 250ms,
                              CaptureAvailability::unavailable)},
      ready, start + 250ms);
  expect(engine.on_measurements(
             std::vector{measurement(true, start + 300ms)}, ready,
             start + 300ms)
             .empty(),
         "repeat mode should rebuild stability after capture recovery");
  expect(engine.on_measurements(
             std::vector{measurement(true, start + 400ms)}, ready,
             start + 400ms) ==
             std::vector<VisualTriggerRequest>{{
                 "dash", VisualTriggerSource::pixel_repeat}},
         "repeat mode may fire only after recovered pixels become stable again");
  (void)engine.on_measurements(
      std::vector{measurement(true, start + 500ms)}, cooling, start + 500ms);
  expect(engine.on_measurements(
             std::vector{measurement(true, start + 1s)}, ready, start + 1s) ==
             std::vector<VisualTriggerRequest>{{
                 "dash", VisualTriggerSource::pixel_repeat}},
         "repeat mode should fire again after the cooldown returns to full");
}

} // namespace layerc::test
