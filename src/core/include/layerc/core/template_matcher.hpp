#pragma once

#include "layerc/core/pixel_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace layerc::core {

// Longest accepted template side, shared by matching, asset codecs, and
// calibration downscaling.
inline constexpr std::uint32_t max_template_axis = 512U;

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

// Converts bounded, interleaved RGBA bytes into a validated matcher image.
[[nodiscard]] std::optional<TemplateImage> template_image_from_rgba(
    std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgba);

// Centered search area twice the selection on each axis, within the frame.
[[nodiscard]] NormalizedRegion
expanded_template_search_region(NormalizedRegion selected);

// Samples the source pixel under the center of each downscaled pixel so the
// image never exceeds max_template_axis on its longer side. Nullopt for an
// invalid frame or a selection covering fewer than 64 captured pixels.
[[nodiscard]] std::optional<TemplateImage>
template_from_selection(PixelFrameView frame, const NormalizedRegion &selection);

struct TemplateMatchResult {
  bool matched{};
  // True when the configured search region intersects pixels supplied by the
  // capture backend. Budget exhaustion and unsupported references do not
  // masquerade as capture being outside the output.
  bool search_region_available{};
  double confidence{};
  std::string reference_sha256;
  std::size_t candidates_evaluated{};
  // Candidates that reached a final confidence score. Zero means no evidence
  // exists, which is distinct from a candidate that scored zero.
  std::size_t candidates_scored{};
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

// Bounded masked CPU matcher used by LayerD and LayerCal. The implementation
// caches quality-specific reference samples by verified content hash.
class CpuTemplateMatcher final {
public:
  CpuTemplateMatcher();
  ~CpuTemplateMatcher();
  CpuTemplateMatcher(CpuTemplateMatcher &&) noexcept;
  CpuTemplateMatcher &operator=(CpuTemplateMatcher &&) noexcept;
  CpuTemplateMatcher(const CpuTemplateMatcher &) = delete;
  CpuTemplateMatcher &operator=(const CpuTemplateMatcher &) = delete;

  [[nodiscard]] TemplateMatchResult
  evaluate(const TemplateState &state, std::span<const TemplateImage> references,
           PixelFrameView frame, CaptureQuality quality,
           NormalizedRegion available_region,
           std::span<const NormalizedRegion> unavailable_regions);
  void clear();
  // Cumulative count of quality-specific reference preparations. Useful for
  // confirming that a retained configuration does not repeat CPU work.
  [[nodiscard]] std::size_t preparation_count() const noexcept;
  [[nodiscard]] std::uint64_t prepared_cache_bytes() const noexcept;
  // Drops prepared references whose content hash is not listed. Prepared
  // entries are content-addressed, so a reconfigure need not discard the rest.
  void retain(std::span<const std::string> asset_sha256);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace layerc::core
