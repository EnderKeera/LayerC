#pragma once

#include "layerc/capture/capture.hpp"
#include "layerc/core/template_matcher.hpp"
#include "layerc/core/visual.hpp"

#include <span>

namespace layerc::capture {

// Immutable evaluation context shared by native capture hosts. Acquisition,
// scheduling, reference preparation and thread ownership remain in the hosts.
struct VisualEvaluationContext {
  const CapturePollResult &capture;
  core::CaptureQuality quality;
  core::TimePoint sampled_at;
};

[[nodiscard]] core::ObservationMeasurement evaluate_pixel_measurement(
    const core::PixelRule &rule, const VisualEvaluationContext &context);

[[nodiscard]] core::ObservationMeasurement evaluate_template_measurement(
    const core::TemplateState &state,
    std::span<const core::TemplateImage> references, bool assets_available,
    const VisualEvaluationContext &context, core::CpuTemplateMatcher &matcher);

} // namespace layerc::capture
