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
  void invalidate() noexcept { observation_.reset(); }

  void observe(const PixelRule &rule, const PixelEvaluation &result,
               const CaptureQuality quality, const bool region_available) {
    invalidate();
    if (region_available && result.sampled_pixels != 0) {
      observation_.emplace(rule);
      quality_ = quality;
    }
  }

  void observe(const TemplateState &state, const TemplateMatchResult &result,
               const CaptureQuality quality) {
    invalidate();
    if (result.search_region_available && result.candidates_scored != 0) {
      observation_.emplace(state);
      quality_ = quality;
    }
  }

  [[nodiscard]] bool ready_for(const VisualObservation &observation,
                               const CaptureQuality quality) const {
    return observation_ && quality_ == quality && *observation_ == observation;
  }

private:
  std::optional<VisualObservation> observation_;
  CaptureQuality quality_{CaptureQuality::balanced};
};

} // namespace layerc::core
