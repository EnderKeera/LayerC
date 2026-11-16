#include "layerc/core/calibration_readiness.hpp"

namespace layerc::core {
// Out-of-line to avoid GCC 14 false positives for nested optional/variant state.
void CalibrationReadiness::invalidate() noexcept { observation_.reset(); }

void CalibrationReadiness::observe(const PixelRule &rule, const PixelEvaluation &result,
             const CaptureQuality quality, const bool region_available) {
  invalidate();
  if (region_available && result.sampled_pixels != 0) {
    observation_.emplace(rule);
    quality_ = quality;
  }
}

void CalibrationReadiness::observe(const TemplateState &state, const TemplateMatchResult &result,
             const CaptureQuality quality) {
  invalidate();
  if (result.search_region_available && result.candidates_scored != 0) {
    observation_.emplace(state);
    quality_ = quality;
  }
}

bool CalibrationReadiness::ready_for(const VisualObservation &observation,
                             const CaptureQuality quality) const {
  return observation_ && quality_ == quality && *observation_ == observation;
}

} // namespace layerc::core
