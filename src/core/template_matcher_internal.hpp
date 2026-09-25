#pragma once

#include "layerc/core/template_matcher.hpp"

#include <vector>

namespace layerc::core::detail {

struct TemplateMatcherSample {
  float x{};
  float y{};
  RgbPixel color;
  float luma{};
};

struct TemplateMatcherReference {
  std::vector<TemplateMatcherSample> samples;
  double mean_luma{};
  double luma_variance{};
  std::vector<TemplateMatcherSample> search_samples;
  double search_mean_luma{};
  double search_luma_variance{};
  std::vector<TemplateMatcherSample> locator_samples;
  double locator_mean_luma{};
  double locator_luma_variance{};
};

[[nodiscard]] TemplateMatcherReference prepare_template(const TemplateImage &image,
                                                         CaptureQuality quality);

} // namespace layerc::core::detail
