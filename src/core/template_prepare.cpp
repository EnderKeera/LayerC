#include "template_matcher_internal.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace layerc::core {
namespace {

constexpr std::size_t minimum_mask_pixels = 64;
constexpr std::size_t locator_sample_count = 3;
constexpr double minimum_template_luma_variance = 4.0;

std::size_t search_sample_limit(const CaptureQuality quality) noexcept {
  return quality_profile(quality).search_samples;
}

std::uint32_t axis_bound(const CaptureQuality quality,
                         const TemplateImage &image) noexcept {
  const auto bound = quality_profile(quality).template_axis;
  return bound == 0 ? std::max(image.width, image.height) : bound;
}

using PreparedSample = detail::TemplateMatcherSample;
using PreparedTemplate = detail::TemplateMatcherReference;

void calculate_statistics(const std::span<const PreparedSample> samples,
                          double &mean_luma, double &luma_variance) {
  mean_luma = 0.0;
  luma_variance = 0.0;
  if (samples.empty()) return;
  for (const auto &sample : samples) mean_luma += sample.luma;
  mean_luma /= samples.size();
  for (const auto &sample : samples) {
    const auto delta = sample.luma - mean_luma;
    luma_variance += delta * delta;
  }
}

} // namespace

namespace detail {

PreparedTemplate prepare_template(const TemplateImage &image,
                                  const CaptureQuality quality) {
  PreparedTemplate output;
  const auto bound = axis_bound(quality, image);
  const double factor =
      std::min(1.0, static_cast<double>(bound) /
                        std::max(image.width, image.height));
  const auto samples_x = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(image.width * factor)));
  const auto samples_y = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(image.height * factor)));
  output.samples.reserve(static_cast<std::size_t>(samples_x) * samples_y);
  for (std::uint32_t row = 0; row < samples_y; ++row) {
    const auto source_y = std::min(
        image.height - 1,
        static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(row) * 2U + 1U) * image.height /
            (static_cast<std::uint64_t>(samples_y) * 2U)));
    for (std::uint32_t column = 0; column < samples_x; ++column) {
      const auto source_x = std::min(
          image.width - 1,
          static_cast<std::uint32_t>(
              (static_cast<std::uint64_t>(column) * 2U + 1U) * image.width /
              (static_cast<std::uint64_t>(samples_x) * 2U)));
      const auto index = static_cast<std::size_t>(source_y) * image.width +
                         source_x;
      if (image.alpha[index] == 0) continue;
      const auto pixel = image.pixels[index];
      output.samples.push_back(
          {static_cast<float>((static_cast<double>(column) + 0.5) / samples_x),
           static_cast<float>((static_cast<double>(row) + 0.5) / samples_y),
           pixel, static_cast<float>(rgb_luminance(pixel))});
    }
  }

  // A thin alpha mask can fall between every downsample cell. Preserve useful
  // masked references by falling back to a bounded scan of opaque source
  // pixels rather than incorrectly treating the template as empty.
  if (output.samples.size() < minimum_mask_pixels) {
    output.samples.clear();
    const auto maximum_samples =
        static_cast<std::size_t>(bound) * static_cast<std::size_t>(bound);
    const auto opaque = static_cast<std::size_t>(std::count_if(
        image.alpha.begin(), image.alpha.end(),
        [](const std::uint8_t value) { return value != 0; }));
    const auto keep_every =
        std::max<std::size_t>(1, (opaque + maximum_samples - 1) /
                                    std::max<std::size_t>(1, maximum_samples));
    std::size_t seen{};
    for (std::uint32_t y = 0; y < image.height; ++y) {
      for (std::uint32_t x = 0; x < image.width; ++x) {
        const auto index = static_cast<std::size_t>(y) * image.width + x;
        if (image.alpha[index] == 0 || seen++ % keep_every != 0) continue;
        const auto pixel = image.pixels[index];
        output.samples.push_back(
            {static_cast<float>((static_cast<double>(x) + 0.5) / image.width),
             static_cast<float>((static_cast<double>(y) + 0.5) / image.height),
             pixel, static_cast<float>(rgb_luminance(pixel))});
      }
    }
  }

  if (output.samples.empty()) return output;
  calculate_statistics(output.samples, output.mean_luma,
                        output.luma_variance);
  if (output.luma_variance / output.samples.size() <
      minimum_template_luma_variance) {
    output.samples.clear();
    return output;
  }
  const auto limit = search_sample_limit(quality);
  if (output.samples.size() <= limit) {
    output.search_samples = output.samples;
  } else {
    output.search_samples.reserve(limit);
    for (std::size_t index = 0; index < limit; ++index) {
      // index < limit < size keeps source below size without clamping.
      const auto source =
          (index * output.samples.size() + output.samples.size() / 2U) /
          limit;
      output.search_samples.push_back(output.samples[source]);
    }
  }
  calculate_statistics(output.search_samples, output.search_mean_luma,
                       output.search_luma_variance);
  output.locator_samples.reserve(locator_sample_count);
  static_assert(locator_sample_count > 1);
  for (std::size_t index = 0; index < locator_sample_count; ++index) {
    const auto source = index * (output.search_samples.size() - 1U) /
                        (locator_sample_count - 1U);
    output.locator_samples.push_back(output.search_samples[source]);
  }
  calculate_statistics(output.locator_samples, output.locator_mean_luma,
                       output.locator_luma_variance);
  return output;
}

} // namespace detail

bool TemplateImage::valid() const noexcept {
  if (width == 0 || height == 0 || width > max_template_axis ||
      height > max_template_axis ||
      pixels.size() != static_cast<std::size_t>(width) * height ||
      alpha.size() != pixels.size())
    return false;
  return static_cast<std::size_t>(std::count_if(
             alpha.begin(), alpha.end(),
             [](const std::uint8_t value) { return value != 0; })) >=
         minimum_mask_pixels;
}

std::optional<TemplateImage> template_image_from_rgba(
    const std::uint32_t width, const std::uint32_t height,
    const std::span<const std::uint8_t> rgba) {
  if (width == 0 || height == 0 || width > max_template_axis ||
      height > max_template_axis ||
      rgba.size() != static_cast<std::size_t>(width) * height * 4U)
    return std::nullopt;
  TemplateImage image;
  image.width = width;
  image.height = height;
  image.pixels.resize(static_cast<std::size_t>(width) * height);
  image.alpha.resize(image.pixels.size());
  for (std::size_t index = 0; index < image.pixels.size(); ++index) {
    image.pixels[index] = {rgba[index * 4U], rgba[index * 4U + 1U],
                           rgba[index * 4U + 2U]};
    image.alpha[index] = rgba[index * 4U + 3U];
  }
  if (!image.valid()) return std::nullopt;
  return image;
}

NormalizedRegion expanded_template_search_region(const NormalizedRegion selected) {
  const auto expand = [](const double origin, const double extent) {
    const double expanded = std::min(1.0, extent * 2.0);
    const double center = origin + extent * 0.5;
    return std::pair{std::clamp(center - expanded * 0.5, 0.0, 1.0 - expanded),
                     expanded};
  };
  const auto [x, width] = expand(selected.x, selected.width);
  const auto [y, height] = expand(selected.y, selected.height);
  return {x, y, width, height};
}

std::optional<TemplateImage>
template_from_selection(const PixelFrameView frame,
                        const NormalizedRegion &selection) {
  if (!frame.valid()) return std::nullopt;
  const auto [left, top, right, bottom] =
      to_pixel_bounds(selection, frame.width, frame.height);
  if (right <= left || bottom <= top ||
      static_cast<std::uint64_t>(right - left) * (bottom - top) <
          minimum_mask_pixels)
    return std::nullopt;
  const auto source_width = right - left, source_height = bottom - top;
  const auto source_offset = [](const std::uint32_t index,
                                const std::uint32_t extent,
                                const std::uint32_t source_extent) {
    return std::min(source_extent - 1,
                    static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(index) * 2U + 1U) *
                        source_extent / (static_cast<std::uint64_t>(extent) * 2U)));
  };
  const double factor =
      std::min(1.0, static_cast<double>(max_template_axis) /
                        std::max(source_width, source_height));
  TemplateImage image;
  image.width = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(source_width * factor)));
  image.height = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(source_height * factor)));
  image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);
  image.alpha.assign(image.pixels.size(), 255);
  for (std::uint32_t y = 0; y < image.height; ++y) {
    const auto source_y = top + source_offset(y, image.height, source_height);
    for (std::uint32_t x = 0; x < image.width; ++x) {
      const auto source_x = left + source_offset(x, image.width, source_width);
      image.pixels[static_cast<std::size_t>(y) * image.width + x] =
          frame.pixels[static_cast<std::size_t>(source_y) * frame.stride() +
                       source_x];
    }
  }
  return image;
}

bool template_image_has_structure(const TemplateImage &image) noexcept {
  if (!image.valid()) return false;
  double mean{};
  double squared_delta{};
  std::size_t count{};
  for (std::size_t index = 0; index < image.pixels.size(); ++index) {
    if (image.alpha[index] == 0) continue;
    ++count;
    const double value = rgb_luminance(image.pixels[index]);
    const double delta = value - mean;
    mean += delta / static_cast<double>(count);
    squared_delta += delta * (value - mean);
  }
  return count >= minimum_mask_pixels &&
         squared_delta / static_cast<double>(count) >=
             minimum_template_luma_variance;
}

bool detail::template_is_compact(const TemplateImage &image,
                                 const CaptureQuality quality) noexcept {
  return !image.valid() || quality == CaptureQuality::exact ||
         std::max(image.width, image.height) <= axis_bound(quality, image);
}

TemplateImage compact_template_image(const TemplateImage &image,
                                     const CaptureQuality quality) {
  if (detail::template_is_compact(image, quality)) return image;
  const auto bound = axis_bound(quality, image);
  const double factor = static_cast<double>(bound) /
                        std::max(image.width, image.height);
  TemplateImage compact;
  compact.asset_sha256 = image.asset_sha256;
  compact.width = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(image.width * factor)));
  compact.height = std::max<std::uint32_t>(
      1, static_cast<std::uint32_t>(std::lround(image.height * factor)));
  const auto count = static_cast<std::size_t>(compact.width) * compact.height;
  compact.pixels.resize(count);
  compact.alpha.assign(count, 0);
  std::vector<std::uint64_t> red(count);
  std::vector<std::uint64_t> green(count);
  std::vector<std::uint64_t> blue(count);
  std::vector<std::uint32_t> samples(count);
  for (std::uint32_t y = 0; y < image.height; ++y) {
    const auto target_y = std::min(
        compact.height - 1,
        static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) *
                                   compact.height / image.height));
    for (std::uint32_t x = 0; x < image.width; ++x) {
      const auto source = static_cast<std::size_t>(y) * image.width + x;
      if (image.alpha[source] == 0) continue;
      const auto target_x = std::min(
          compact.width - 1,
          static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) *
                                     compact.width / image.width));
      const auto target =
          static_cast<std::size_t>(target_y) * compact.width + target_x;
      red[target] += image.pixels[source].red;
      green[target] += image.pixels[source].green;
      blue[target] += image.pixels[source].blue;
      compact.alpha[target] = std::max(compact.alpha[target],
                                       image.alpha[source]);
      ++samples[target];
    }
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (samples[index] == 0) continue;
    compact.pixels[index] = {
        static_cast<std::uint8_t>(red[index] / samples[index]),
        static_cast<std::uint8_t>(green[index] / samples[index]),
        static_cast<std::uint8_t>(blue[index] / samples[index])};
  }
  // Very sparse masks can collapse below the 64-sample safety floor. Retain
  // the original only for that exceptional reference so matching remains
  // correct rather than silently weakening its alpha mask.
  return compact.valid() ? compact : image;
}

} // namespace layerc::core
