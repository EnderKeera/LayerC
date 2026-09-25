#include "layerc/capture/visual_evaluation.hpp"

#include "layerc/core/pixel_engine.hpp"

namespace layerc::capture {

core::ObservationMeasurement evaluate_pixel_measurement(
    const core::PixelRule &rule, const VisualEvaluationContext &context) {
  const auto &capture = context.capture;
  auto availability = capture.availability;
  bool matched{};
  if (availability == core::CaptureAvailability::available && capture.frame) {
    if (!region_is_available(rule.region, *capture.frame)) {
      availability = core::CaptureAvailability::outside_output;
    } else {
      matched =
          core::evaluate_pixel_rule(rule, capture.frame->view(), context.quality)
              .matched;
    }
  }
  return {.observation_id = rule.id,
          .availability = availability,
          .matched = matched,
          .confidence = matched ? 1.0 : 0.0,
          .sampled_at = context.sampled_at};
}

core::ObservationMeasurement evaluate_template_measurement(
    const core::TemplateState &state,
    const std::span<const core::TemplateImage> references,
    const bool assets_available, const VisualEvaluationContext &context,
    core::CpuTemplateMatcher &matcher) {
  const auto &capture = context.capture;
  auto availability = capture.availability;
  bool matched{};
  double confidence{};
  if (!assets_available || references.size() != state.references.size()) {
    availability = core::CaptureAvailability::unavailable;
  } else if (availability == core::CaptureAvailability::available &&
             capture.frame) {
    const auto result = matcher.evaluate(state, references,
                                         capture.frame->view(), context.quality,
                                         capture.frame->available_region,
                                         capture.frame->unavailable_regions);
    if (!result.search_region_available) {
      availability = core::CaptureAvailability::outside_output;
    } else {
      matched = result.matched;
      confidence = result.confidence;
    }
  }
  return {.observation_id = state.id,
          .availability = availability,
          .matched = matched,
          .confidence = confidence,
          .sampled_at = context.sampled_at};
}

} // namespace layerc::capture
