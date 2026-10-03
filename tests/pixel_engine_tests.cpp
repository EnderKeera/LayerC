#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/pixel_engine.hpp"
#include "layerc/core/template_matcher.hpp"

#include <chrono>
#include <iostream>
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
  profile.observations.push_back(layerc::core::PixelRule{.id = "ready",
                           .name = "Ready",
                           .region = {0.0, 0.0, 1.0, 1.0},
                           .predicate = AverageColorPredicate{0x20A040, 8},
                           .stable_samples = 2,
                           .repeat = repeat});
  profile.triggers.push_back({.cooldown_id = "dash",
                              .observation_id = "ready",
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
  return {.observation_id = "ready",
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

void test_template_matching() {
  using namespace layerc::core;
  constexpr std::uint32_t frame_axis = 64;
  constexpr std::uint32_t template_axis = 10;
  std::vector<RgbPixel> pixels(frame_axis * frame_axis, {12, 18, 24});
  TemplateImage reference{.asset_sha256 = std::string(64, 'a'),
                          .width = template_axis,
                          .height = template_axis,
                          .pixels = std::vector<RgbPixel>(
                              template_axis * template_axis),
                          .alpha = std::vector<std::uint8_t>(
                              template_axis * template_axis, 0)};
  for (std::uint32_t y = 1; y < 9; ++y) {
    for (std::uint32_t x = 1; x < 9; ++x) {
      const RgbPixel value{
          static_cast<std::uint8_t>(20 + x * 19),
          static_cast<std::uint8_t>(30 + y * 17),
          static_cast<std::uint8_t>(40 + (x + y) * 9)};
      reference.pixels[y * template_axis + x] = value;
      reference.alpha[y * template_axis + x] = 255;
      pixels[(24 + y) * frame_axis + (18 + x)] = value;
    }
  }
  expect(reference.valid(),
         "a bounded template with 64 opaque pixels should be valid");
  expect(template_image_has_structure(reference),
         "a varied template should carry enough spatial structure to match");

  TemplateImage flat{.asset_sha256 = std::string(64, 'f'),
                     .width = 8,
                     .height = 8,
                     .pixels = std::vector<RgbPixel>(64, {80, 186, 200}),
                     .alpha = std::vector<std::uint8_t>(64, 255)};
  expect(flat.valid() && !template_image_has_structure(flat),
         "a syntactically valid flat image should be rejected as a positional template");
  std::vector<RgbPixel> flat_frame(frame_axis * frame_axis, {80, 186, 200});
  TemplateState flat_state{
      .id = "flat",
      .name = "Flat",
      .search_region = {0.0, 0.0, 1.0, 1.0},
      .expected_width = 0.125,
      .expected_height = 0.125,
      .references = {{flat.asset_sha256}},
      .confidence_threshold = 0.90,
      .color_tolerance = 32};
  CpuTemplateMatcher flat_matcher;
  const auto flat_result = flat_matcher.evaluate(
      flat_state, std::span<const TemplateImage>(&flat, 1),
      PixelFrameView{flat_frame, frame_axis, frame_axis, 0},
      CaptureQuality::balanced, {0.0, 0.0, 1.0, 1.0},
      std::span<const NormalizedRegion>{});
  expect(!flat_result.matched && flat_result.search_region_available &&
             flat_result.candidates_evaluated == 0,
         "flat references must fail closed without being mislabeled as outside captured output");

  TemplateState state{
      .id = "ready-template",
      .name = "Ready template",
      .search_region = {0.0, 0.0, 1.0, 1.0},
      .expected_width = static_cast<double>(template_axis) / frame_axis,
      .expected_height = static_cast<double>(template_axis) / frame_axis,
      .references = {{reference.asset_sha256}},
      .confidence_threshold = 0.98,
      .scale_tolerance = 0.0,
      .color_tolerance = 0};
  CpuTemplateMatcher matcher;
  const auto matched = matcher.evaluate(
      state, std::span<const TemplateImage>(&reference, 1),
      PixelFrameView{pixels, frame_axis, frame_axis, 0},
      CaptureQuality::exact, {0.0, 0.0, 1.0, 1.0},
      std::span<const NormalizedRegion>{});
  expect(matched.matched && matched.confidence >= 0.98 &&
             matched.reference_sha256 == reference.asset_sha256,
         "template matching should find a masked reference in its search region");
  expect(matched.pixel_comparisons <= 8U * 1024U * 1024U,
         "exact template matching should still enforce its hard work budget");

  for (auto &pixel : pixels) pixel = {200, 4, 220};
  const auto rejected = matcher.evaluate(
      state, std::span<const TemplateImage>(&reference, 1),
      PixelFrameView{pixels, frame_axis, frame_axis, 0},
      CaptureQuality::balanced, {0.0, 0.0, 1.0, 1.0},
      std::span<const NormalizedRegion>{});
  expect(!rejected.matched,
         "template matching should reject a structurally and chromatically different frame");

  auto invalid = reference;
  invalid.alpha.pop_back();
  expect(!invalid.valid(),
         "template assets with incomplete alpha masks should be rejected");

  TemplateImage large{.asset_sha256 = std::string(64, 'b'),
                      .width = 128,
                      .height = 64,
                      .pixels = std::vector<RgbPixel>(128U * 64U,
                                                     {40, 80, 120}),
                      .alpha = std::vector<std::uint8_t>(128U * 64U, 255)};
  const auto balanced =
      compact_template_image(large, CaptureQuality::balanced);
  const auto low = compact_template_image(large, CaptureQuality::low);
  expect(balanced.valid() && balanced.width == 64 && balanced.height == 32 &&
             balanced.asset_sha256 == large.asset_sha256 && low.valid() &&
             low.width == 32 && low.height == 16,
         "runtime references should compact to the selected quality bound while preserving identity and masks");

  constexpr std::uint32_t search_width = 640;
  constexpr std::uint32_t search_height = 360;
  constexpr std::uint32_t off_grid_width = 40;
  constexpr std::uint32_t off_grid_height = 28;
  constexpr std::uint32_t off_grid_left = 317;
  constexpr std::uint32_t off_grid_top = 173;
  std::vector<RgbPixel> search_pixels(search_width * search_height,
                                      {7, 11, 17});
  TemplateImage off_grid{
      .asset_sha256 = std::string(64, 'c'),
      .width = off_grid_width,
      .height = off_grid_height,
      .pixels = std::vector<RgbPixel>(off_grid_width * off_grid_height),
      .alpha = std::vector<std::uint8_t>(off_grid_width * off_grid_height,
                                        255)};
  for (std::uint32_t y = 0; y < off_grid_height; ++y) {
    for (std::uint32_t x = 0; x < off_grid_width; ++x) {
      const RgbPixel pixel{
          static_cast<std::uint8_t>((x * 29U + y * 3U) % 251U),
          static_cast<std::uint8_t>((x * 7U + y * 31U) % 253U),
          static_cast<std::uint8_t>((x * 17U + y * 11U) % 247U)};
      off_grid.pixels[static_cast<std::size_t>(y) * off_grid_width + x] =
          pixel;
      search_pixels[static_cast<std::size_t>(off_grid_top + y) *
                        search_width +
                    off_grid_left + x] = pixel;
    }
  }
  TemplateState off_grid_state{
      .id = "off-grid",
      .name = "Off-grid",
      .search_region = {0.0, 0.0, 1.0, 1.0},
      .expected_width =
          static_cast<double>(off_grid_width) / search_width,
      .expected_height =
          static_cast<double>(off_grid_height) / search_height,
      .references = {{off_grid.asset_sha256}},
      .confidence_threshold = 0.98,
      .scale_tolerance = 0.0,
      .color_tolerance = 0};
  CpuTemplateMatcher off_grid_matcher;
  const NormalizedRegion unrelated_exclusion{0.0, 0.0, 0.1, 0.1};
  const auto refined = off_grid_matcher.evaluate(
      off_grid_state, std::span<const TemplateImage>(&off_grid, 1),
      PixelFrameView{search_pixels, search_width, search_height, 0},
      CaptureQuality::balanced,
      {0.0, 0.0, 1.0, 1.0},
      std::span<const NormalizedRegion>(&unrelated_exclusion, 1));
  if (!refined.matched || refined.confidence < 0.98 ||
      refined.pixel_comparisons > 1024U * 1024U) {
    std::cerr << "off-grid confidence=" << refined.confidence
              << " candidates=" << refined.candidates_evaluated
              << " comparisons=" << refined.pixel_comparisons << '\n';
  }
  expect(refined.matched && refined.confidence >= 0.98 &&
             refined.pixel_comparisons <= 1024U * 1024U,
         "balanced matching should refine an off-grid template while respecting its work budget and unrelated exclusions");

  auto intermediate_scale_state = off_grid_state;
  constexpr double rendered_scale = 0.95;
  intermediate_scale_state.expected_width =
      static_cast<double>(off_grid_width) / search_width / rendered_scale;
  intermediate_scale_state.expected_height =
      static_cast<double>(off_grid_height) / search_height / rendered_scale;
  intermediate_scale_state.scale_tolerance = 0.20;
  const auto intermediate_scale = off_grid_matcher.evaluate(
      intermediate_scale_state, std::span<const TemplateImage>(&off_grid, 1),
      PixelFrameView{search_pixels, search_width, search_height, 0},
      CaptureQuality::balanced, {0.0, 0.0, 1.0, 1.0},
      std::span<const NormalizedRegion>{});
  if (!intermediate_scale.matched || intermediate_scale.confidence < 0.98 ||
      intermediate_scale.pixel_comparisons > 1024U * 1024U) {
    std::cerr << "intermediate-scale confidence="
              << intermediate_scale.confidence
              << " candidates=" << intermediate_scale.candidates_evaluated
              << " comparisons=" << intermediate_scale.pixel_comparisons
              << '\n';
  }
  expect(intermediate_scale.matched &&
             intermediate_scale.confidence >= 0.98 &&
             intermediate_scale.pixel_comparisons <= 1024U * 1024U,
         "balanced matching should search a pixel-distinct 0.95 scale that lies between the coarse tolerance anchors without exceeding its work budget");

  const NormalizedRegion covered{
      static_cast<double>(off_grid_left) / search_width,
      static_cast<double>(off_grid_top) / search_height,
      static_cast<double>(off_grid_width) / search_width,
      static_cast<double>(off_grid_height) / search_height};
  const auto excluded = off_grid_matcher.evaluate(
      off_grid_state, std::span<const TemplateImage>(&off_grid, 1),
      PixelFrameView{search_pixels, search_width, search_height, 0},
      CaptureQuality::balanced, {0.0, 0.0, 1.0, 1.0},
      std::span<const NormalizedRegion>(&covered, 1));
  expect(!excluded.matched,
         "template candidates overlapping capture-excluded overlay pixels must not be evaluated as real image data");
  expect(excluded.search_region_available,
         "an excluded candidate region should remain capture-available instead of enabling input fallback");

  const NormalizedRegion all_excluded{0.0, 0.0, 1.0, 1.0};
  const auto no_candidates = off_grid_matcher.evaluate(
      off_grid_state, std::span<const TemplateImage>(&off_grid, 1),
      PixelFrameView{search_pixels, search_width, search_height, 0},
      CaptureQuality::balanced, {0.0, 0.0, 1.0, 1.0},
      std::span<const NormalizedRegion>(&all_excluded, 1));
  expect(no_candidates.search_region_available &&
             no_candidates.candidates_evaluated == 0 &&
             !no_candidates.matched,
         "zero candidates caused by exclusions should fail closed without being mislabeled as outside captured output");

  // A zero threshold is valid, but it must not turn "nothing scored" into a
  // match; a real candidate can still satisfy it.
  auto zero_threshold_state = off_grid_state;
  zero_threshold_state.confidence_threshold = 0.0;
  CpuTemplateMatcher zero_threshold_matcher;
  const auto zero_no_candidates = zero_threshold_matcher.evaluate(
      zero_threshold_state, std::span<const TemplateImage>(&off_grid, 1),
      PixelFrameView{search_pixels, search_width, search_height, 0},
      CaptureQuality::balanced, {0.0, 0.0, 1.0, 1.0},
      std::span<const NormalizedRegion>(&all_excluded, 1));
  expect(zero_no_candidates.search_region_available &&
             zero_no_candidates.candidates_scored == 0 &&
             !zero_no_candidates.matched,
         "a zero threshold must not match when no template candidate was scored");
  const auto zero_scored = zero_threshold_matcher.evaluate(
      zero_threshold_state, std::span<const TemplateImage>(&off_grid, 1),
      PixelFrameView{search_pixels, search_width, search_height, 0},
      CaptureQuality::balanced, {0.0, 0.0, 1.0, 1.0},
      std::span<const NormalizedRegion>(&unrelated_exclusion, 1));
  expect(zero_scored.candidates_scored > 0 && zero_scored.matched &&
             !zero_scored.reference_sha256.empty(),
         "a scored candidate still satisfies a zero threshold and names its reference");

  auto outside_state = off_grid_state;
  outside_state.search_region = {0.4, 0.4, 0.5, 0.5};
  const NormalizedRegion different_output{0.0, 0.0, 0.1, 0.1};
  const auto outside = off_grid_matcher.evaluate(
      outside_state, std::span<const TemplateImage>(&off_grid, 1),
      PixelFrameView{search_pixels, search_width, search_height, 0},
      CaptureQuality::balanced, different_output,
      std::span<const NormalizedRegion>{});
  expect(!outside.search_region_available && outside.candidates_evaluated == 0,
         "only a search region outside the supplied capture output should report outside-output status");
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

  engine.set_visual_enabled(false);
  engine.set_visual_enabled(true);
  expect(engine.on_measurements(
                    std::vector{measurement(true, start + 700ms)}, ready,
                    start + 700ms)
             .empty(),
         "toggling visual input should preserve the configured stability count");
  expect(engine.on_measurements(
                    std::vector{measurement(true, start + 800ms)}, ready,
                    start + 800ms)
                 .size() == 1,
         "a reset engine should fire after the configured stability count");

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
