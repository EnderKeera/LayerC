#include "layerc/core/template_matcher.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace layerc::core {
namespace {

constexpr std::size_t minimum_mask_pixels = 64;
constexpr std::size_t refinement_candidates = 8;
constexpr std::size_t coarse_candidates = 32;
constexpr std::size_t locator_sample_count = 3;
constexpr double minimum_template_luma_variance = 4.0;

std::size_t search_sample_limit(const CaptureQuality quality) noexcept {
  if (quality == CaptureQuality::low) return 128;
  if (quality == CaptureQuality::balanced) return 256;
  return 512;
}

double luminance(const RgbPixel pixel) noexcept {
  return 0.2126 * pixel.red + 0.7152 * pixel.green + 0.0722 * pixel.blue;
}

std::uint32_t axis_bound(const CaptureQuality quality,
                         const TemplateImage &image) noexcept {
  if (quality == CaptureQuality::low) return 32;
  if (quality == CaptureQuality::balanced) return 64;
  return std::max(image.width, image.height);
}

std::size_t comparison_budget(const CaptureQuality quality) noexcept {
  if (quality == CaptureQuality::low) return 512U * 1024U;
  if (quality == CaptureQuality::balanced) return 1024U * 1024U;
  return 8U * 1024U * 1024U;
}

struct PreparedSample {
  float x{};
  float y{};
  RgbPixel color;
  float luma{};
};

struct PreparedTemplate {
  std::string hash;
  std::vector<PreparedSample> samples;
  double mean_luma{};
  double luma_variance{};
  std::vector<PreparedSample> search_samples;
  double search_mean_luma{};
  double search_luma_variance{};
  std::vector<PreparedSample> locator_samples;
  double locator_mean_luma{};
  double locator_luma_variance{};
};

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

PreparedTemplate prepare_template(const TemplateImage &image,
                                  const CaptureQuality quality) {
  PreparedTemplate output;
  output.hash = image.asset_sha256;
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
           pixel, static_cast<float>(luminance(pixel))});
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
             pixel, static_cast<float>(luminance(pixel))});
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
      const auto source = std::min(
          output.samples.size() - 1,
          (index * output.samples.size() + output.samples.size() / 2U) /
              limit);
      output.search_samples.push_back(output.samples[source]);
    }
  }
  calculate_statistics(output.search_samples, output.search_mean_luma,
                       output.search_luma_variance);
  output.locator_samples.reserve(locator_sample_count);
  for (std::size_t index = 0; index < locator_sample_count; ++index) {
    const auto source = locator_sample_count == 1
                            ? output.search_samples.size() / 2U
                            : index * (output.search_samples.size() - 1U) /
                                  (locator_sample_count - 1U);
    output.locator_samples.push_back(
        output.search_samples[std::min(source,
                                       output.search_samples.size() - 1U)]);
  }
  calculate_statistics(output.locator_samples, output.locator_mean_luma,
                       output.locator_luma_variance);
  return output;
}

struct Candidate {
  double structure{};
  std::uint32_t left{};
  std::uint32_t top{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t coarse_step{1};
};

double structure_score(const std::span<const PreparedSample> samples,
                       const double reference_mean,
                       const double reference_variance,
                       const PixelFrameView frame, const std::uint32_t left,
                       const std::uint32_t top, const std::uint32_t width,
                       const std::uint32_t height) {
  const auto stride = frame.row_stride == 0 ? frame.width : frame.row_stride;
  double target_sum{};
  double target_square_sum{};
  double covariance{};
  for (const auto &sample : samples) {
    const auto x = std::min(
        frame.width - 1,
        left + static_cast<std::uint32_t>(sample.x * width));
    const auto y = std::min(
        frame.height - 1,
        top + static_cast<std::uint32_t>(sample.y * height));
    const auto target =
        luminance(frame.pixels[static_cast<std::size_t>(y) * stride + x]);
    target_sum += target;
    target_square_sum += target * target;
    covariance += (sample.luma - reference_mean) * target;
  }
  const double count = static_cast<double>(samples.size());
  const double target_mean = target_sum / count;
  const double target_variance =
      std::max(0.0, target_square_sum - count * target_mean * target_mean);
  if (reference_variance <= 1e-9)
    return std::clamp(
        1.0 - std::abs(reference_mean - target_mean) / 255.0, 0.0,
        1.0);
  if (target_variance <= 1e-9) return 0.0;
  const double correlation =
      covariance / std::sqrt(reference_variance * target_variance);
  return std::clamp((correlation + 1.0) * 0.5, 0.0, 1.0);
}

bool regions_overlap(const NormalizedRegion &left,
                     const NormalizedRegion &right) noexcept {
  constexpr double epsilon = 1e-12;
  return left.x < right.x + right.width - epsilon &&
         left.x + left.width > right.x + epsilon &&
         left.y < right.y + right.height - epsilon &&
         left.y + left.height > right.y + epsilon;
}

bool candidate_is_available(
    const Candidate &candidate, const PixelFrameView frame,
    const NormalizedRegion available_region,
    const std::span<const NormalizedRegion> unavailable_regions) noexcept {
  constexpr double epsilon = 1e-12;
  const NormalizedRegion region{
      static_cast<double>(candidate.left) / frame.width,
      static_cast<double>(candidate.top) / frame.height,
      static_cast<double>(candidate.width) / frame.width,
      static_cast<double>(candidate.height) / frame.height};
  if (region.x + epsilon < available_region.x ||
      region.y + epsilon < available_region.y ||
      region.x + region.width >
          available_region.x + available_region.width + epsilon ||
      region.y + region.height >
          available_region.y + available_region.height + epsilon)
    return false;
  return std::ranges::none_of(unavailable_regions, [&](const auto &excluded) {
    return regions_overlap(region, excluded);
  });
}

double color_coverage(const PreparedTemplate &reference,
                      const PixelFrameView frame, const Candidate &candidate,
                      const std::uint8_t tolerance) {
  const auto stride = frame.row_stride == 0 ? frame.width : frame.row_stride;
  std::size_t matching{};
  for (const auto &sample : reference.samples) {
    const auto x = std::min(
        frame.width - 1,
        candidate.left +
            static_cast<std::uint32_t>(sample.x * candidate.width));
    const auto y = std::min(
        frame.height - 1,
        candidate.top +
            static_cast<std::uint32_t>(sample.y * candidate.height));
    const auto target =
        frame.pixels[static_cast<std::size_t>(y) * stride + x];
    const auto within = [tolerance](const std::uint8_t left,
                                    const std::uint8_t right) {
      return std::abs(static_cast<int>(left) - static_cast<int>(right)) <=
             tolerance;
    };
    if (within(target.red, sample.color.red) &&
        within(target.green, sample.color.green) &&
        within(target.blue, sample.color.blue))
      ++matching;
  }
  return static_cast<double>(matching) / reference.samples.size();
}

double locator_score(const std::span<const PreparedSample> samples,
                     const double reference_mean,
                     const double reference_variance,
                     const PixelFrameView frame,
                     const Candidate &candidate) {
  const auto stride = frame.row_stride == 0 ? frame.width : frame.row_stride;
  double target_sum{};
  double target_square_sum{};
  double covariance{};
  double difference{};
  for (const auto &sample : samples) {
    const auto x = std::min(
        frame.width - 1,
        candidate.left +
            static_cast<std::uint32_t>(sample.x * candidate.width));
    const auto y = std::min(
        frame.height - 1,
        candidate.top +
            static_cast<std::uint32_t>(sample.y * candidate.height));
    const auto target =
        frame.pixels[static_cast<std::size_t>(y) * stride + x];
    const auto target_luma = luminance(target);
    target_sum += target_luma;
    target_square_sum += target_luma * target_luma;
    covariance += (sample.luma - reference_mean) * target_luma;
    difference +=
        std::abs(static_cast<int>(target.red) - sample.color.red) +
        std::abs(static_cast<int>(target.green) - sample.color.green) +
        std::abs(static_cast<int>(target.blue) - sample.color.blue);
  }
  const double count = static_cast<double>(samples.size());
  const double target_mean = target_sum / count;
  const double target_variance =
      std::max(0.0, target_square_sum - count * target_mean * target_mean);
  double structure{};
  if (reference_variance <= 1e-9) {
    structure = std::clamp(
        1.0 - std::abs(reference_mean - target_mean) / 255.0, 0.0, 1.0);
  } else if (target_variance > 1e-9) {
    structure = std::clamp(
        (covariance / std::sqrt(reference_variance * target_variance) + 1.0) *
            0.5,
        0.0, 1.0);
  }
  const auto color = std::clamp(
      1.0 - difference /
                (count * 3.0 * 255.0),
      0.0, 1.0);
  return 0.5 * structure + 0.5 * color;
}

bool color_is_near(const RgbPixel left, const RgbPixel right,
                   const std::uint8_t tolerance) noexcept {
  const auto within = [tolerance](const std::uint8_t a,
                                  const std::uint8_t b) {
    return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= tolerance;
  };
  return within(left.red, right.red) && within(left.green, right.green) &&
         within(left.blue, right.blue);
}

struct RenderedSize {
  std::uint32_t width{};
  std::uint32_t height{};
};

std::vector<RenderedSize> rendered_sizes(
    const TemplateState &state, const PixelFrameView frame,
    const std::uint32_t maximum_width,
    const std::uint32_t maximum_height) {
  const double base_width = state.expected_width * frame.width;
  const double base_height = state.expected_height * frame.height;
  if (base_width <= 0.0 || base_height <= 0.0) return {};

  const double minimum_scale = std::max(0.01, 1.0 - state.scale_tolerance);
  const double maximum_scale = 1.0 + state.scale_tolerance;
  std::vector<double> boundaries{minimum_scale, maximum_scale};
  const auto append_boundaries = [&](const double base) {
    const auto last = static_cast<std::uint32_t>(
        std::ceil(base * maximum_scale) + 1.0);
    for (std::uint32_t value = 0; value <= last; ++value) {
      const double boundary = (static_cast<double>(value) + 0.5) / base;
      if (boundary > minimum_scale && boundary < maximum_scale)
        boundaries.push_back(boundary);
    }
  };
  append_boundaries(base_width);
  append_boundaries(base_height);
  std::ranges::sort(boundaries);
  boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                   boundaries.end());

  std::vector<RenderedSize> sizes;
  std::unordered_set<std::uint64_t> seen;
  const auto append_size = [&](const double scale) {
    const auto width = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(std::lround(base_width * scale)));
    const auto height = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(std::lround(base_height * scale)));
    if (width > maximum_width || height > maximum_height) return;
    const auto key = (static_cast<std::uint64_t>(width) << 32U) | height;
    if (seen.insert(key).second) sizes.push_back({width, height});
  };
  append_size(minimum_scale);
  for (std::size_t index = 1; index < boundaries.size(); ++index)
    append_size((boundaries[index - 1] + boundaries[index]) * 0.5);
  append_size(maximum_scale);
  append_size(std::clamp(1.0, minimum_scale, maximum_scale));

  std::ranges::stable_sort(sizes, [&](const auto &left, const auto &right) {
    const auto distance = [&](const RenderedSize &size) {
      return std::abs(static_cast<double>(size.width) - base_width) +
             std::abs(static_cast<double>(size.height) - base_height);
    };
    return distance(left) < distance(right);
  });
  return sizes;
}

} // namespace

bool TemplateImage::valid() const noexcept {
  if (width == 0 || height == 0 || width > 512 || height > 512 ||
      pixels.size() != static_cast<std::size_t>(width) * height ||
      alpha.size() != pixels.size())
    return false;
  return static_cast<std::size_t>(std::count_if(
             alpha.begin(), alpha.end(),
             [](const std::uint8_t value) { return value != 0; })) >=
         minimum_mask_pixels;
}

bool template_image_has_structure(const TemplateImage &image) noexcept {
  if (!image.valid()) return false;
  double mean{};
  double squared_delta{};
  std::size_t count{};
  for (std::size_t index = 0; index < image.pixels.size(); ++index) {
    if (image.alpha[index] == 0) continue;
    ++count;
    const double value = luminance(image.pixels[index]);
    const double delta = value - mean;
    mean += delta / static_cast<double>(count);
    squared_delta += delta * (value - mean);
  }
  return count >= minimum_mask_pixels &&
         squared_delta / static_cast<double>(count) >=
             minimum_template_luma_variance;
}

TemplateImage compact_template_image(const TemplateImage &image,
                                     const CaptureQuality quality) {
  if (!image.valid() || quality == CaptureQuality::exact) return image;
  const auto bound = axis_bound(quality, image);
  if (std::max(image.width, image.height) <= bound) return image;
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

class CpuTemplateMatcher::Impl {
public:
  TemplateMatchResult evaluate(const TemplateState &state,
                               const std::span<const TemplateImage> images,
                               const PixelFrameView frame,
                               const CaptureQuality quality,
                               const NormalizedRegion available_region,
                               const std::span<const NormalizedRegion>
                                   unavailable_regions) {
    TemplateMatchResult result;
    if (!frame.valid()) return result;
    const auto stride = frame.row_stride == 0 ? frame.width : frame.row_stride;
    (void)stride;
    const auto left = static_cast<std::uint32_t>(std::clamp(
        std::floor(state.search_region.x * frame.width), 0.0,
        static_cast<double>(frame.width)));
    const auto top = static_cast<std::uint32_t>(std::clamp(
        std::floor(state.search_region.y * frame.height), 0.0,
        static_cast<double>(frame.height)));
    const auto right = static_cast<std::uint32_t>(std::clamp(
        std::ceil((state.search_region.x + state.search_region.width) *
                  frame.width),
        0.0, static_cast<double>(frame.width)));
    const auto bottom = static_cast<std::uint32_t>(std::clamp(
        std::ceil((state.search_region.y + state.search_region.height) *
                  frame.height),
        0.0, static_cast<double>(frame.height)));
    if (right <= left || bottom <= top) return result;
    const NormalizedRegion search_region{
        static_cast<double>(left) / frame.width,
        static_cast<double>(top) / frame.height,
        static_cast<double>(right - left) / frame.width,
        static_cast<double>(bottom - top) / frame.height};
    result.search_region_available =
        regions_overlap(search_region, available_region);

    const auto budget = comparison_budget(quality);
    std::size_t reference_index{};
    for (const auto &definition : state.references) {
      ++reference_index;
      const auto image = std::ranges::find(
          images, definition.asset_sha256, &TemplateImage::asset_sha256);
      if (image == images.end() || !image->valid()) continue;
      const auto key = definition.asset_sha256 + ":" +
                       std::to_string(static_cast<unsigned>(quality));
      auto [prepared_it, inserted] = prepared_.try_emplace(key);
      if (inserted) {
        const auto compact = compact_template_image(*image, quality);
        prepared_it->second = prepare_template(compact, quality);
      }
      const auto &reference = prepared_it->second;
      if (reference.samples.empty() || reference.search_samples.empty() ||
          reference.locator_samples.empty())
        continue;

      std::vector<Candidate> candidates;
      candidates.reserve(coarse_candidates);
      const auto sizes = rendered_sizes(state, frame, right - left,
                                        bottom - top);
      if (sizes.empty()) continue;
      const auto references_remaining =
          std::max<std::size_t>(1, state.references.size() -
                                      reference_index + 1U);
      const auto remaining_budget =
          budget > result.pixel_comparisons
              ? budget - result.pixel_comparisons
              : 0U;
      const auto reference_start = result.pixel_comparisons;
      const auto reference_budget =
          result.pixel_comparisons + remaining_budget / references_remaining;
      const auto reference_allowance =
          reference_budget - result.pixel_comparisons;
      const auto final_candidate_cost =
          reference.samples.size() * 2U;
      const auto final_reserve =
          std::min<std::size_t>(
              refinement_candidates,
              final_candidate_cost == 0
                  ? 0
                  : reference_allowance / final_candidate_cost) *
          final_candidate_cost;
      const auto search_budget =
          reference_budget >
                  result.pixel_comparisons + final_reserve
              ? reference_budget - result.pixel_comparisons - final_reserve
              : reference_allowance / 2U;
      const auto coarse_budget = search_budget * 3U / 4U;
      const auto coarse_limit = reference_start + coarse_budget;
      const auto refinement_limit = reference_start + search_budget;
      std::size_t early_validation_work{};

      const auto keep_candidate = [&](Candidate candidate) {
        if (candidates.size() < coarse_candidates) {
          candidates.push_back(std::move(candidate));
          return;
        }
        const auto weakest =
            std::ranges::min_element(candidates, {}, &Candidate::structure);
        if (weakest != candidates.end() &&
            candidate.structure > weakest->structure)
          *weakest = std::move(candidate);
      };

      const auto score_candidate = [&](Candidate candidate,
                                       const std::size_t limit,
                                       bool &exhausted) -> double {
        if (result.pixel_comparisons + reference.locator_samples.size() >
                limit + early_validation_work ||
            result.pixel_comparisons + reference.locator_samples.size() >
                reference_budget) {
          exhausted = true;
          return -1.0;
        }
        if (!candidate_is_available(candidate, frame, available_region,
                                    unavailable_regions))
          return -1.0;
        candidate.structure = locator_score(
            reference.locator_samples, reference.locator_mean_luma,
            reference.locator_luma_variance, frame, candidate);
        // locator_score fuses structure and color into one pixel pass, so this
        // is the complete coarse-stage work charge.
        result.pixel_comparisons += reference.locator_samples.size();
        ++result.candidates_evaluated;
        const double score = candidate.structure;
        keep_candidate(std::move(candidate));
        return score;
      };
      const auto finalize_candidate = [&](Candidate &candidate,
                                          const bool early) {
        if (result.pixel_comparisons + final_candidate_cost >
            reference_budget)
          return false;
        candidate.structure = structure_score(
            reference.samples, reference.mean_luma,
            reference.luma_variance, frame, candidate.left, candidate.top,
            candidate.width, candidate.height);
        result.pixel_comparisons += reference.samples.size();
        const double coverage = color_coverage(
            reference, frame, candidate, state.color_tolerance);
        result.pixel_comparisons += reference.samples.size();
        if (early) early_validation_work += final_candidate_cost;
        const double confidence =
            0.70 * candidate.structure + 0.30 * coverage;
        if (confidence > result.confidence) {
          result.confidence = confidence;
          result.reference_sha256 = definition.asset_sha256;
        }
        return true;
      };

      // Find an anchor color across the region once and project every
      // pixel-distinct allowed size back to a candidate origin. Intermediate
      // scales are therefore searchable without repeating a full positional
      // scan for every size.
      const auto locator_cost = reference.locator_samples.size();
      const auto scale_reserve = std::min<std::size_t>(
          coarse_budget, locator_cost * sizes.size());
      const auto anchor_budget =
          coarse_budget > scale_reserve
              ? (coarse_budget - scale_reserve) / 2U
              : 0U;
      const auto anchor_limit = reference_start + anchor_budget;
      const auto anchor_allowance =
          anchor_limit > result.pixel_comparisons
              ? anchor_limit - result.pixel_comparisons
              : 0U;
      const auto search_pixels = static_cast<std::uint64_t>(right - left) *
                                 (bottom - top);
      const auto anchor_step = std::max<std::uint32_t>(
          1, anchor_allowance == 0
                 ? std::numeric_limits<std::uint32_t>::max()
                 : static_cast<std::uint32_t>(std::ceil(std::sqrt(
                       static_cast<double>(search_pixels) /
                       static_cast<double>(anchor_allowance)))));
      const auto &anchor = reference.locator_samples.front();
      bool anchor_exhausted = anchor_allowance == 0;
      for (std::uint32_t y = top; y < bottom && !anchor_exhausted;
           y += anchor_step) {
        for (std::uint32_t x = left; x < right; x += anchor_step) {
          if (result.pixel_comparisons + 1U >
              anchor_limit + early_validation_work) {
            anchor_exhausted = true;
            break;
          }
          const auto pixel =
              frame.pixels[static_cast<std::size_t>(y) * stride + x];
          ++result.pixel_comparisons;
          if (!color_is_near(pixel, anchor.color, state.color_tolerance))
            continue;
          for (const auto size : sizes) {
            const auto anchor_x = static_cast<std::uint32_t>(
                anchor.x * static_cast<float>(size.width));
            const auto anchor_y = static_cast<std::uint32_t>(
                anchor.y * static_cast<float>(size.height));
            const auto candidate_left =
                static_cast<std::int64_t>(x) - anchor_x;
            const auto candidate_top =
                static_cast<std::int64_t>(y) - anchor_y;
            if (candidate_left < left || candidate_top < top ||
                candidate_left + size.width > right ||
                candidate_top + size.height > bottom)
              continue;
            Candidate candidate{
                .left = static_cast<std::uint32_t>(candidate_left),
                .top = static_cast<std::uint32_t>(candidate_top),
                .width = size.width,
                .height = size.height,
                .coarse_step = anchor_step};
            const double score = score_candidate(candidate, anchor_limit,
                                                  anchor_exhausted);
            // A coarse locator is only a prefilter. Validate one promising
            // anchor candidate against the full reference immediately; return
            // early only when that position meets the configured confidence.
            if (score >= 0.90 && early_validation_work == 0U &&
                finalize_candidate(candidate, true) &&
                result.confidence >= state.confidence_threshold) {
              result.matched = true;
              return result;
            }
            if (anchor_exhausted) break;
          }
          if (anchor_exhausted) break;
        }
      }

      for (std::size_t size_index = 0; size_index < sizes.size();
           ++size_index) {
        const auto size = sizes[size_index];
        const auto width = size.width;
        const auto height = size.height;
        const auto positions_x = right - left - width + 1U;
        const auto positions_y = bottom - top - height + 1U;
        const auto remaining_coarse =
            coarse_limit + early_validation_work > result.pixel_comparisons
                ? coarse_limit + early_validation_work -
                      result.pixel_comparisons
                : 0U;
        if (remaining_coarse < reference.locator_samples.size()) break;
        const auto sizes_remaining = sizes.size() - size_index;
        const auto candidate_cap = std::max<std::size_t>(
            1, remaining_coarse /
                   (reference.locator_samples.size() *
                    std::max<std::size_t>(1, sizes_remaining)));
        const auto total_positions =
            static_cast<std::uint64_t>(positions_x) * positions_y;
        const auto step = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(std::ceil(std::sqrt(
                   static_cast<double>(total_positions) / candidate_cap))));
        const auto score_position = [&](const std::uint32_t x,
                                        const std::uint32_t y,
                                        bool &exhausted) {
          Candidate candidate{.left = left + x,
                              .top = top + y,
                              .width = width,
                              .height = height,
                              .coarse_step = step};
          (void)score_candidate(std::move(candidate), coarse_limit,
                                exhausted);
        };

        bool exhausted{};
        for (std::uint32_t y = 0; y < positions_y && !exhausted; y += step) {
          for (std::uint32_t x = 0; x < positions_x; x += step) {
            score_position(x, y, exhausted);
            if (exhausted) break;
          }
          if (!exhausted && positions_x > 1U &&
              (positions_x - 1U) % step != 0U)
            score_position(positions_x - 1U, y, exhausted);
        }
        if (!exhausted && positions_y > 1U &&
            (positions_y - 1U) % step != 0U) {
          const auto last_y = positions_y - 1U;
          for (std::uint32_t x = 0; x < positions_x; x += step) {
            score_position(x, last_y, exhausted);
            if (exhausted) break;
          }
          if (!exhausted && positions_x > 1U &&
              (positions_x - 1U) % step != 0U)
            score_position(positions_x - 1U, last_y, exhausted);
        }
      }

      // Refine each strong coarse candidate with a logarithmic hill search.
      // This reaches pixel precision without spending the bounded budget on a
      // full-resolution exhaustive scan.
      for (auto &candidate : candidates) {
        auto delta = candidate.coarse_step / 2U;
        while (delta > 0U) {
          Candidate best = candidate;
          for (const int y_direction : {-1, 0, 1}) {
            for (const int x_direction : {-1, 0, 1}) {
              if (x_direction == 0 && y_direction == 0) continue;
              const auto maximum_left = right - candidate.width;
              const auto maximum_top = bottom - candidate.height;
              const auto next_left = static_cast<std::uint32_t>(std::clamp<
                  std::int64_t>(
                  static_cast<std::int64_t>(candidate.left) +
                      static_cast<std::int64_t>(x_direction) * delta,
                  left, maximum_left));
              const auto next_top = static_cast<std::uint32_t>(std::clamp<
                  std::int64_t>(
                  static_cast<std::int64_t>(candidate.top) +
                      static_cast<std::int64_t>(y_direction) * delta,
                  top, maximum_top));
              if (next_left == candidate.left &&
                  next_top == candidate.top)
                continue;
              if (result.pixel_comparisons +
                      reference.search_samples.size() >
                  refinement_limit + early_validation_work)
                break;
              Candidate next{.left = next_left,
                             .top = next_top,
                             .width = candidate.width,
                             .height = candidate.height,
                             .coarse_step = 1};
              if (!candidate_is_available(next, frame, available_region,
                                          unavailable_regions))
                continue;
              next.structure = structure_score(
                  reference.search_samples, reference.search_mean_luma,
                  reference.search_luma_variance, frame, next.left, next.top,
                  next.width, next.height);
              result.pixel_comparisons += reference.search_samples.size();
              ++result.candidates_evaluated;
              if (next.structure > best.structure) best = next;
            }
          }
          candidate = best;
          if (delta == 1U) break;
          delta = std::max<std::uint32_t>(1, delta / 2U);
        }
      }
      std::ranges::sort(candidates, std::greater{}, &Candidate::structure);

      std::size_t finalized{};
      for (auto &candidate : candidates) {
        if (finalized++ >= refinement_candidates) break;
        if (!finalize_candidate(candidate, false)) break;
      }
      if (result.pixel_comparisons >= budget) break;
    }
    result.matched = result.confidence >= state.confidence_threshold;
    return result;
  }

  void clear() { prepared_.clear(); }

private:
  std::unordered_map<std::string, PreparedTemplate> prepared_;
};

CpuTemplateMatcher::CpuTemplateMatcher() : impl_(std::make_unique<Impl>()) {}
CpuTemplateMatcher::~CpuTemplateMatcher() = default;
CpuTemplateMatcher::CpuTemplateMatcher(CpuTemplateMatcher &&) noexcept =
    default;
CpuTemplateMatcher &
CpuTemplateMatcher::operator=(CpuTemplateMatcher &&) noexcept = default;

TemplateMatchResult CpuTemplateMatcher::evaluate(
    const TemplateState &state, const std::span<const TemplateImage> references,
    const PixelFrameView frame, const CaptureQuality quality,
    const NormalizedRegion available_region,
    const std::span<const NormalizedRegion> unavailable_regions) {
  return impl_->evaluate(state, references, frame, quality, available_region,
                         unavailable_regions);
}

void CpuTemplateMatcher::clear() { impl_->clear(); }

} // namespace layerc::core
