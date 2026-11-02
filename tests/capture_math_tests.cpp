#include "test_support.hpp"

#include "layerc/capture/capture.hpp"
#include "layerc/capture/visual_evaluation.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#if defined(_WIN32)
#include "layerc/capture/window_capture.hpp"
#endif

#include <cstdlib>
#include <iostream>
#include <span>
#include <type_traits>
#include <utility>

void capture_math_contracts() {
  using layerc::capture::region_is_available;
  using layerc::core::NormalizedRegion;
  using layerc::test::expect;
  expect(region_is_available({0.2, 0.2, 0.2, 0.2},
                             {0.1, 0.1, 0.8, 0.8}),
         "a rule inside the fallback output should remain available");
  expect(!region_is_available({0.0, 0.2, 0.2, 0.2},
                              {0.1, 0.1, 0.8, 0.8}),
         "a rule crossing the fallback output should be unavailable");
  expect(region_is_available({0.0, 0.0, 1.0, 1.0},
                             {0.0, 0.0, 1.0, 1.0}),
         "a full WGC frame should contain every normalized rule");
  constexpr double epsilon = 1e-9;
  expect(region_is_available({0.1 - epsilon * 0.5, 0.1 - epsilon * 0.5,
                              0.2, 0.2},
                             {0.1, 0.1, 0.8, 0.8}),
         "availability tolerance should expand the left and top symmetrically");
  expect(!region_is_available({0.1 - epsilon * 2.0, 0.1, 0.2, 0.2},
                              {0.1, 0.1, 0.8, 0.8}),
         "regions beyond the symmetric tolerance should remain unavailable");
  expect(layerc::capture::capture_sample_dimensions(
             1920, 1080, layerc::core::CaptureQuality::low) ==
             std::pair<std::uint32_t, std::uint32_t>{64, 36},
         "low capture should use an aspect-preserving 64-pixel bound");
  expect(layerc::capture::capture_sample_dimensions(
             3840, 2160, layerc::core::CaptureQuality::balanced) ==
             std::pair<std::uint32_t, std::uint32_t>{256, 144},
         "balanced capture should use an aspect-preserving 256-pixel bound");
  expect(layerc::capture::capture_sample_dimensions(
             3840, 2160, layerc::core::CaptureQuality::exact) ==
             std::pair<std::uint32_t, std::uint32_t>{3840, 2160},
         "exact capture should retain every source pixel");
  using layerc::capture::OutputRotation;
  using layerc::capture::PixelBounds;
  using layerc::capture::transform_output_crop;
  const PixelBounds crop{100, 200, 300, 500};
  expect(transform_output_crop(crop, 1080, 1920,
                               OutputRotation::identity) == crop,
         "identity outputs should retain logical crop coordinates");
  expect(transform_output_crop(crop, 1080, 1920,
                               OutputRotation::rotate90) ==
             PixelBounds{200, 780, 500, 980},
         "90-degree outputs should swap axes and reverse logical X");
  expect(transform_output_crop(crop, 1080, 1920,
                               OutputRotation::rotate180) ==
             PixelBounds{780, 1420, 980, 1720},
         "180-degree outputs should reverse both axes");
  expect(transform_output_crop(crop, 1080, 1920,
                               OutputRotation::rotate270) ==
             PixelBounds{1420, 100, 1720, 300},
         "270-degree outputs should swap axes and reverse logical Y");

  layerc::capture::CaptureFrame frame;
  frame.available_region = {0.0, 0.0, 1.0, 1.0};
  frame.unavailable_regions.push_back({0.4, 0.4, 0.2, 0.2});
  expect(!region_is_available({0.45, 0.45, 0.1, 0.1}, frame),
         "rules overlapping excluded overlay pixels should be unavailable");
  expect(region_is_available({0.2, 0.2, 0.2, 0.2}, frame),
         "rules touching but not entering an excluded region should remain available");

  static_assert(std::is_same_v<
                decltype(layerc::capture::CapturePoll::window),
                layerc::runtime::WindowId>);
  const layerc::runtime::WindowId window{42};
  auto valid_frame = std::make_shared<layerc::capture::CaptureFrame>();
  valid_frame->width = 2;
  valid_frame->height = 2;
  valid_frame->pixels.resize(4);
  auto valid_poll = layerc::capture::normalize_capture_poll(
      window, {.availability = layerc::core::CaptureAvailability::available,
               .backend = layerc::core::CaptureBackendKind::none,
               .diagnostic = "valid",
               .frame = valid_frame});
  expect(valid_poll.window == window &&
             valid_poll.result.availability ==
                 layerc::core::CaptureAvailability::available &&
             valid_poll.result.frame == valid_frame &&
             valid_poll.result.diagnostic == "valid",
         "the keyed seam should preserve a valid available frame");

  auto short_frame = std::make_shared<layerc::capture::CaptureFrame>(
      *valid_frame);
  short_frame->pixels.pop_back();
  const auto short_poll = layerc::capture::normalize_capture_poll(
      window, {.availability = layerc::core::CaptureAvailability::available,
               .frame = short_frame});
  expect(short_poll.result.availability ==
                 layerc::core::CaptureAvailability::unavailable &&
             !short_poll.result.frame,
         "an undersized available frame should become unavailable");

  auto oversized_frame = std::make_shared<layerc::capture::CaptureFrame>(
      *valid_frame);
  oversized_frame->pixels.push_back({});
  const auto oversized_poll = layerc::capture::normalize_capture_poll(
      window, {.availability = layerc::core::CaptureAvailability::available,
               .frame = oversized_frame});
  expect(oversized_poll.result.availability ==
                 layerc::core::CaptureAvailability::unavailable &&
             !oversized_poll.result.frame,
         "a tightly packed frame should reject pixels outside its bounds");

  // Only a capture addressed by the target's own native identity is
  // target_bound; the shared seam keeps whatever the backend reported.
  const layerc::capture::CapturePollResult default_binding;
  expect(default_binding.binding ==
                 layerc::capture::CaptureTargetBinding::target_bound,
         "capture results default to a target-bound association");
  for (const auto binding :
       {layerc::capture::CaptureTargetBinding::monitor_crop,
        layerc::capture::CaptureTargetBinding::user_selected_window}) {
    const auto preserved = layerc::capture::normalize_capture_poll(
        window, {.availability = layerc::core::CaptureAvailability::unavailable,
                 .backend = layerc::core::CaptureBackendKind::pipewire_portal,
                 .diagnostic = "portal_pending",
                 .frame = {},
                 .recovery = layerc::capture::CaptureRecovery::none,
                 .binding = binding});
    expect(preserved.result.binding == binding,
           "the shared capture seam preserves the backend's target binding");
  }

  const auto missing_poll = layerc::capture::normalize_capture_poll(
      window, {.availability = layerc::core::CaptureAvailability::available});
  expect(missing_poll.result.availability ==
                 layerc::core::CaptureAvailability::unavailable &&
             !missing_poll.result.frame,
         "an available result without a frame should become unavailable");

  const layerc::core::CaptureAvailability unavailable_states[]{
      layerc::core::CaptureAvailability::inactive,
      layerc::core::CaptureAvailability::starting,
      layerc::core::CaptureAvailability::unavailable,
      layerc::core::CaptureAvailability::protected_or_black,
      layerc::core::CaptureAvailability::outside_output,
  };
  for (const auto availability : unavailable_states) {
    const auto unavailable_poll = layerc::capture::normalize_capture_poll(
        window, {.availability = availability,
                 .backend = layerc::core::CaptureBackendKind::none,
                 .diagnostic = "preserved",
                 .frame = valid_frame});
    expect(unavailable_poll.result.availability == availability &&
               !unavailable_poll.result.frame &&
               unavailable_poll.result.diagnostic == "preserved",
           "unavailable states should clear stale frames only");
  }

  layerc::capture::CaptureFrame black_frame;
  black_frame.width = 512;
  black_frame.height = 512;
  black_frame.pixels.resize(
      static_cast<std::size_t>(black_frame.width) * black_frame.height);
  expect(layerc::capture::frame_is_nearly_black(black_frame),
         "a genuinely black frame should remain protected");
  layerc::capture::CaptureHealthTracker health;
  auto observed_black = std::make_shared<layerc::capture::CaptureFrame>();
  observed_black->width = 1;
  observed_black->height = 1;
  observed_black->pixels.resize(1);
  auto health_result = health.observe(
      {.availability = layerc::core::CaptureAvailability::available,
       .frame = observed_black});
  (void)health.observe(
      {.availability = layerc::core::CaptureAvailability::available,
       .frame = observed_black});
  expect(health.consecutive_black_frames() == 1,
         "polling one buffered frame twice should count it once");
  for (std::uint32_t index = 1;
       index < layerc::capture::CaptureHealthTracker::protected_frame_threshold;
       ++index) {
    auto next_black =
        std::make_shared<layerc::capture::CaptureFrame>(*observed_black);
    health_result = health.observe(
        {.availability = layerc::core::CaptureAvailability::available,
         .frame = std::move(next_black)});
  }
  expect(health_result.availability ==
                 layerc::core::CaptureAvailability::protected_or_black &&
             health_result.diagnostic.find("persistent black") !=
                 std::string::npos,
         "one shared threshold should classify persistent black frames");
  health.reset();
  auto observed_white = std::make_shared<layerc::capture::CaptureFrame>();
  observed_white->width = 1;
  observed_white->height = 1;
  observed_white->pixels.push_back({255, 255, 255});
  health_result = health.observe(
      {.availability = layerc::core::CaptureAvailability::available,
       .frame = observed_white});
  expect(health_result.availability ==
                 layerc::core::CaptureAvailability::available &&
             health.consecutive_black_frames() == 0,
         "reset and a non-black frame should return capture health to live");
  const layerc::core::RgbPixel white{255, 255, 255};
  for (std::uint32_t x = 0; x < black_frame.width; ++x) {
    black_frame.pixels[x] = white;
    black_frame.pixels[(black_frame.height - 1U) * black_frame.width + x] =
        white;
  }
  for (std::uint32_t y = 0; y < black_frame.height; ++y) {
    black_frame.pixels[static_cast<std::size_t>(y) * black_frame.width] = white;
    black_frame.pixels[static_cast<std::size_t>(y) * black_frame.width +
                       black_frame.width - 1U] = white;
  }
  expect(!layerc::capture::frame_is_nearly_black(black_frame),
         "sparse bright window borders should not be missed by black-frame sampling");

#if defined(_WIN32)
  layerc::capture::WindowCaptureSession session(nullptr);
  const auto now = layerc::core::MonotonicClock::now();
  const auto polled = session.poll(now, false);
  expect(polled.window == layerc::runtime::WindowId{} &&
             polled.result.availability ==
                 layerc::core::CaptureAvailability::unavailable &&
             polled.result.backend == layerc::core::CaptureBackendKind::none &&
             polled.result.diagnostic == "capture is not started" &&
             polled.result.frame == nullptr,
         "a session that was never started reports a normalized, "
         "diagnosed unavailable poll");
#endif

  // F9 shared synchronous evaluator: one ROI/error-to-measurement policy
  // for the Windows worker and the Linux inline service.
  {
    using layerc::core::CaptureAvailability;
    using layerc::core::CaptureQuality;
    const auto sampled_at = layerc::core::MonotonicClock::now();
    layerc::core::CpuTemplateMatcher matcher;
    const layerc::core::PixelRule white_rule{
        .id = "white",
        .name = "White",
        .region = {0.0, 0.0, 1.0, 1.0},
        .predicate = layerc::core::AverageColorPredicate{
            .target_color = 0xFFFFFF, .tolerance = 16}};

    // Unavailable frames keep their availability and never match.
    const layerc::capture::CapturePollResult unavailable{
        .availability = CaptureAvailability::unavailable};
    const auto unavailable_measurement =
        layerc::capture::evaluate_pixel_measurement(
            white_rule, {unavailable, CaptureQuality::low, sampled_at});
    expect(unavailable_measurement.availability ==
                   CaptureAvailability::unavailable &&
               !unavailable_measurement.matched &&
               unavailable_measurement.sampled_at == sampled_at,
           "an unavailable frame must stay unavailable with caller time");

    // Rules outside the frame map to outside_output, not unavailable.
    auto narrow_frame = std::make_shared<layerc::capture::CaptureFrame>();
    narrow_frame->width = 2;
    narrow_frame->height = 2;
    narrow_frame->pixels.assign(4, layerc::core::RgbPixel{255, 255, 255});
    narrow_frame->available_region = {0.0, 0.0, 0.5, 0.5};
    const layerc::capture::CapturePollResult narrow{
        .availability = CaptureAvailability::available,
        .frame = narrow_frame};
    const layerc::core::PixelRule far_rule{
        .id = "far",
        .name = "Far",
        .region = {0.75, 0.75, 0.2, 0.2},
        .predicate = layerc::core::AverageColorPredicate{
            .target_color = 0xFFFFFF, .tolerance = 16}};
    expect(layerc::capture::evaluate_pixel_measurement(
               far_rule, {narrow, CaptureQuality::low, sampled_at})
                   .availability == CaptureAvailability::outside_output,
           "a rule outside the available region must report outside_output");

    // A matching pixel rule reports matched with full confidence.
    auto white_frame = std::make_shared<layerc::capture::CaptureFrame>();
    white_frame->width = 2;
    white_frame->height = 2;
    white_frame->pixels.assign(4, layerc::core::RgbPixel{255, 255, 255});
    const layerc::capture::CapturePollResult white_capture{
        .availability = CaptureAvailability::available, .frame = white_frame};
    const auto matched = layerc::capture::evaluate_pixel_measurement(
        white_rule, {white_capture, CaptureQuality::low, sampled_at});
    expect(matched.matched && matched.confidence == 1.0 &&
               matched.availability == CaptureAvailability::available &&
               matched.sampled_at == sampled_at,
           "a matching pixel rule must report matched with caller time");

    // Missing template references fail as unavailable, never as a match.
    const layerc::core::TemplateState template_state{
        .id = "sigil",
        .name = "Sigil",
        .search_region = {0.0, 0.0, 1.0, 1.0},
        .references = {{.asset_sha256 = std::string(64, 'a')}}};
    const std::span<const layerc::core::TemplateImage> no_references{};
    const auto reference_failure =
        layerc::capture::evaluate_template_measurement(
            template_state, no_references, false,
            {white_capture, CaptureQuality::low, sampled_at}, matcher);
    expect(reference_failure.availability ==
                   CaptureAvailability::unavailable &&
               !reference_failure.matched &&
               reference_failure.sampled_at == sampled_at,
           "missing template assets must report unavailable with caller time");
  }

}

int main() {
  return layerc::test::run(
      {{"capture_math_contracts", capture_math_contracts}});
}
