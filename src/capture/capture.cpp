#include "layerc/capture/capture.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace layerc::capture {
namespace {

bool frame_is_valid(const CaptureFrame &frame) noexcept {
  const auto expected =
      static_cast<std::uint64_t>(frame.width) * frame.height;
  return frame.view().valid() &&
         expected == static_cast<std::uint64_t>(frame.pixels.size());
}

} // namespace

CapturePoll normalize_capture_poll(const core::WindowId window,
                                   CapturePollResult result) {
  if (result.availability != core::CaptureAvailability::available) {
    result.frame.reset();
  } else if (!result.frame || !frame_is_valid(*result.frame)) {
    result.availability = core::CaptureAvailability::unavailable;
    result.frame.reset();
    if (result.diagnostic.empty())
      result.diagnostic = "capture returned an invalid frame";
    else
      result.diagnostic += "; invalid capture frame";
  }
  return {.window = window, .result = std::move(result)};
}

CapturePollResult CaptureHealthTracker::observe(CapturePollResult result) {
  if (result.availability == core::CaptureAvailability::available &&
      result.frame && result.frame != observed_frame_) {
    observed_frame_ = result.frame;
    if (frame_is_nearly_black(*result.frame)) {
      if (black_frames_ < protected_frame_threshold) ++black_frames_;
    } else {
      black_frames_ = 0;
    }
  }
  if (black_frames_ >= protected_frame_threshold && result.frame) {
    result.availability = core::CaptureAvailability::protected_or_black;
    if (!result.diagnostic.empty()) result.diagnostic += "; ";
    result.diagnostic += "persistent black frames detected";
  }
  return result;
}

void CaptureHealthTracker::reset() noexcept {
  observed_frame_.reset();
  black_frames_ = 0;
}

bool frame_is_nearly_black(const CaptureFrame &frame) noexcept {
  if (frame.pixels.empty())
    return true;
  std::size_t bright{};
  std::size_t sampled{};
  const auto sample = [&](const std::size_t index) {
    if (index >= frame.pixels.size())
      return;
    const auto pixel = frame.pixels[index];
    if (pixel.red > 3 || pixel.green > 3 || pixel.blue > 3)
      ++bright;
    ++sampled;
  };

  const auto step = std::max<std::size_t>(1, frame.pixels.size() / 4'096U);
  for (std::size_t index = 0; index < frame.pixels.size(); index += step) {
    sample(index);
  }

  const auto expected_pixels =
      static_cast<std::uint64_t>(frame.width) * frame.height;
  if (frame.width != 0 && frame.height != 0 &&
      expected_pixels <= frame.pixels.size()) {
    // Border oversampling is deliberate: letterboxed captures are black in
    // the middle but carry UI chrome on the edges, so the perimeter gets
    // its own walk. Two plain loops (rows, then columns) instead of one
    // offset→(x,y) mapping.
    const std::size_t width = frame.width;
    const std::size_t height = frame.height;
    const auto row_step = std::max<std::size_t>(1, width / 1'024U);
    for (std::size_t x = 0; x < width; x += row_step) {
      sample(x);
      if (height > 1) sample((height - 1U) * width + x);
    }
    const auto column_step = std::max<std::size_t>(1, height / 1'024U);
    for (std::size_t y = 1; y + 1 < height; y += column_step) {
      sample(y * width);
      if (width > 1) sample(y * width + width - 1U);
    }
  }
  return bright * 1'000U < sampled * 5U;
}

bool region_is_available(
    const core::NormalizedRegion &rule_region,
    const core::NormalizedRegion &available_region) noexcept {
  return core::contains(available_region, rule_region);
}

bool region_is_available(const core::NormalizedRegion &rule_region,
                         const CaptureFrame &frame) noexcept {
  if (!region_is_available(rule_region, frame.available_region))
    return false;
  return std::ranges::none_of(
      frame.unavailable_regions, [&](const core::NormalizedRegion &excluded) {
        return core::overlaps(rule_region, excluded);
      });
}

PixelBounds transform_output_crop(const PixelBounds logical_crop,
                                  const std::uint32_t logical_output_width,
                                  const std::uint32_t logical_output_height,
                                  const OutputRotation rotation) noexcept {
  switch (rotation) {
  case OutputRotation::rotate90:
    return {logical_crop.top, logical_output_width - logical_crop.right,
            logical_crop.bottom, logical_output_width - logical_crop.left};
  case OutputRotation::rotate180:
    return {logical_output_width - logical_crop.right,
            logical_output_height - logical_crop.bottom,
            logical_output_width - logical_crop.left,
            logical_output_height - logical_crop.top};
  case OutputRotation::rotate270:
    return {logical_output_height - logical_crop.bottom, logical_crop.left,
            logical_output_height - logical_crop.top, logical_crop.right};
  case OutputRotation::identity:
  default:
    return logical_crop;
  }
}

std::uint32_t sampled_coordinate(const std::uint32_t index,
                                 const std::uint32_t source_extent,
                                 const std::uint32_t output_extent) noexcept {
  return std::min<std::uint32_t>(
      source_extent - 1U,
      static_cast<std::uint32_t>(
          (static_cast<std::uint64_t>(index) * source_extent +
           source_extent / 2U) /
          output_extent));
}

std::pair<std::uint32_t, std::uint32_t>
capture_sample_dimensions(const std::uint32_t width, const std::uint32_t height,
                          const core::CaptureQuality quality) noexcept {
  if (width == 0 || height == 0)
    return {0, 0};
  std::uint32_t limit{};
  switch (quality) {
  case core::CaptureQuality::low:
    limit = 64U;
    break;
  case core::CaptureQuality::balanced:
    limit = 256U;
    break;
  case core::CaptureQuality::exact:
    limit = 0U;
    break;
  }
  if (limit == 0 || (width <= limit && height <= limit))
    return {width, height};
  const double scale = std::min(static_cast<double>(limit) / width,
                                static_cast<double>(limit) / height);
  return {std::max<std::uint32_t>(
              1, static_cast<std::uint32_t>(std::lround(width * scale))),
          std::max<std::uint32_t>(
              1, static_cast<std::uint32_t>(std::lround(height * scale)))};
}

} // namespace layerc::capture
