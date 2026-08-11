#pragma once

#include "layerc/core/pixel_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace layerc::core {

struct TemplateImage {
  std::string asset_sha256;
  std::uint32_t width{};
  std::uint32_t height{};
  std::vector<RgbPixel> pixels;
  // One alpha byte per pixel. Alpha zero is excluded from matching; every
  // nonzero value participates. Asset validation requires 64 participants.
  std::vector<std::uint8_t> alpha;

  [[nodiscard]] bool valid() const noexcept;
  bool operator==(const TemplateImage &) const = default;
};

struct TemplateMatchResult {
  bool matched{};
  // True when the configured search region intersects pixels supplied by the
  // capture backend. Budget exhaustion and unsupported references do not
  // masquerade as capture being outside the output.
  bool search_region_available{};
  double confidence{};
  std::string reference_sha256;
  std::size_t candidates_evaluated{};
  std::size_t pixel_comparisons{};
};

// Flat and near-flat images do not carry enough spatial information to locate
// an element. They are rejected instead of matching an arbitrary same-colored
// area elsewhere in the search region.
[[nodiscard]] bool
template_image_has_structure(const TemplateImage &image) noexcept;

// Produces the largest representation a quality level can consume. This lets
// long-lived runtimes release full-resolution decoded PNG pixels after
// validation while calibration can continue using the original image.
[[nodiscard]] TemplateImage
compact_template_image(const TemplateImage &image, CaptureQuality quality);

class ITemplateMatcher {
public:
  virtual ~ITemplateMatcher() = default;
  [[nodiscard]] virtual TemplateMatchResult
  evaluate(const TemplateState &state, std::span<const TemplateImage> references,
           PixelFrameView frame, CaptureQuality quality,
           NormalizedRegion available_region = {0.0, 0.0, 1.0, 1.0},
           std::span<const NormalizedRegion> unavailable_regions = {}) = 0;
};

// Bounded masked CPU matcher used by LayerD and LayerCal. The implementation
// caches quality-specific reference samples by verified content hash.
class CpuTemplateMatcher final : public ITemplateMatcher {
public:
  CpuTemplateMatcher();
  ~CpuTemplateMatcher() override;
  CpuTemplateMatcher(CpuTemplateMatcher &&) noexcept;
  CpuTemplateMatcher &operator=(CpuTemplateMatcher &&) noexcept;
  CpuTemplateMatcher(const CpuTemplateMatcher &) = delete;
  CpuTemplateMatcher &operator=(const CpuTemplateMatcher &) = delete;

  [[nodiscard]] TemplateMatchResult
  evaluate(const TemplateState &state, std::span<const TemplateImage> references,
           PixelFrameView frame, CaptureQuality quality,
           NormalizedRegion available_region = {0.0, 0.0, 1.0, 1.0},
           std::span<const NormalizedRegion> unavailable_regions = {}) override;
  void clear();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace layerc::core
