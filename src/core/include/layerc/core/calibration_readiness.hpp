#pragma once

#include "layerc/core/template_matcher.hpp"

#include <optional>

namespace layerc::core {

inline constexpr const char *calibration_sampling_required =
    "Capture a frame and evaluate the current predicate before applying calibration.";

// One authority shared by both native calibrators. This records evaluation,
// not a match: a genuine negative result still tests the selected predicate.
class CalibrationReadiness final {
public:
  void invalidate() noexcept;

  void observe(const PixelRule &rule, const PixelEvaluation &result,
               CaptureQuality quality, bool region_available);
  void observe(const TemplateState &state, const TemplateMatchResult &result,
               CaptureQuality quality);

  [[nodiscard]] bool ready_for(const VisualObservation &observation,
                               CaptureQuality quality) const;

private:
  std::optional<VisualObservation> observation_;
  CaptureQuality quality_{CaptureQuality::balanced};
};

} // namespace layerc::core
