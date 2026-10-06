#include "template_matcher_internal.hpp"

#include "layerc/core/string_hash.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace layerc::core {
namespace {

constexpr std::size_t refinement_candidates = 8;
constexpr std::size_t coarse_candidates = 32;

std::size_t comparison_budget(const CaptureQuality quality) noexcept {
  return quality_profile(quality).comparisons;
}

using PreparedSample = detail::TemplateMatcherSample;
using PreparedTemplate = detail::TemplateMatcherReference;

struct Candidate {
  double structure{};
  std::uint32_t left{};
  std::uint32_t top{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t coarse_step{1};
};

// Maps a Pearson-like correlation of luminance samples to a [0,1] similarity.
// Degenerate (near-constant) references fall back to mean-distance scoring.
double structure_similarity(const double reference_mean,
                            const double reference_variance,
                            const double target_sum,
                            const double target_square_sum,
                            const double covariance, const double count) {
  const double target_mean = target_sum / count;
  const double target_variance =
      std::max(0.0, target_square_sum - count * target_mean * target_mean);
  if (reference_variance <= 1e-9)
    return std::clamp(
        1.0 - std::abs(reference_mean - target_mean) / 255.0, 0.0, 1.0);
  if (target_variance <= 1e-9) return 0.0;
  const double correlation =
      covariance / std::sqrt(reference_variance * target_variance);
  return std::clamp((correlation + 1.0) * 0.5, 0.0, 1.0);
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

template <typename Visit>
void for_each_target_sample(const std::span<const PreparedSample> samples,
                            const PixelFrameView frame,
                            const Candidate &candidate, Visit &&visit) {
  const auto stride = frame.stride();
  for (const auto &sample : samples) {
    const auto x = std::min(frame.width - 1,
        candidate.left + static_cast<std::uint32_t>(sample.x * candidate.width));
    const auto y = std::min(frame.height - 1,
        candidate.top + static_cast<std::uint32_t>(sample.y * candidate.height));
    visit(sample, frame.pixels[static_cast<std::size_t>(y) * stride + x]);
  }
}

double structure_score(const std::span<const PreparedSample> samples,
                       const double reference_mean,
                       const double reference_variance,
                       const PixelFrameView frame, const std::uint32_t left,
                       const std::uint32_t top, const std::uint32_t width,
                       const std::uint32_t height) {
  double target_sum{};
  double target_square_sum{};
  double covariance{};
  for_each_target_sample(samples, frame, {0.0, left, top, width, height},
                         [&](const auto &sample, const auto pixel) {
    const auto target = rgb_luminance(pixel);
    target_sum += target;
    target_square_sum += target * target;
    covariance += (sample.luma - reference_mean) * target;
  });
  return structure_similarity(reference_mean, reference_variance, target_sum,
                              target_square_sum, covariance,
                              static_cast<double>(samples.size()));
}

bool candidate_is_available(
    const Candidate &candidate, const PixelFrameView frame,
    const NormalizedRegion available_region,
    const std::span<const NormalizedRegion> unavailable_regions) noexcept {
  const NormalizedRegion region{
      static_cast<double>(candidate.left) / frame.width,
      static_cast<double>(candidate.top) / frame.height,
      static_cast<double>(candidate.width) / frame.width,
      static_cast<double>(candidate.height) / frame.height};
  if (!contains(available_region, region)) return false;
  return std::ranges::none_of(unavailable_regions, [&](const auto &excluded) {
    return overlaps(region, excluded);
  });
}

double color_coverage(const PreparedTemplate &reference,
                      const PixelFrameView frame, const Candidate &candidate,
                      const std::uint8_t tolerance) {
  std::size_t matching{};
  for_each_target_sample(reference.samples, frame, candidate,
                         [&](const auto &sample, const auto target) {
    if (color_is_near(target, sample.color, tolerance)) ++matching;
  });
  return static_cast<double>(matching) / reference.samples.size();
}

double locator_score(const std::span<const PreparedSample> samples,
                     const double reference_mean,
                     const double reference_variance,
                     const PixelFrameView frame,
                     const Candidate &candidate) {
  double target_sum{};
  double target_square_sum{};
  double covariance{};
  double difference{};
  for_each_target_sample(samples, frame, candidate,
                         [&](const auto &sample, const auto target) {
    const auto target_luma = rgb_luminance(target);
    target_sum += target_luma;
    target_square_sum += target_luma * target_luma;
    covariance += (sample.luma - reference_mean) * target_luma;
    difference +=
        std::abs(static_cast<int>(target.red) - sample.color.red) +
        std::abs(static_cast<int>(target.green) - sample.color.green) +
        std::abs(static_cast<int>(target.blue) - sample.color.blue);
  });
  const double count = static_cast<double>(samples.size());
  const double structure =
      structure_similarity(reference_mean, reference_variance, target_sum,
                           target_square_sum, covariance, count);
  const auto color = std::clamp(
      1.0 - difference / (count * 3.0 * 255.0), 0.0, 1.0);
  return 0.5 * structure + 0.5 * color;
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

namespace {

// One reference's search inputs; [left, right) x [top, bottom) is the frame
// pixel search window.
struct ReferenceSearchInputs {
  const TemplateState &state;
  const TemplateReference &definition;
  const PreparedTemplate &reference;
  PixelFrameView frame;
  NormalizedRegion available_region;
  std::span<const NormalizedRegion> unavailable_regions;
  std::uint32_t left{};
  std::uint32_t top{};
  std::uint32_t right{};
  std::uint32_t bottom{};
  std::span<const RenderedSize> sizes;
  std::size_t total_budget{};
  std::size_t references_remaining{};
};

struct ReferenceSearchContext {
  ReferenceSearchContext(const ReferenceSearchInputs &inputs,
                         TemplateMatchResult &result_value);

  bool run();

private:
  [[nodiscard]] std::size_t calculate_search_budget() const;
  void keep_candidate(Candidate candidate);
  double score_candidate(Candidate candidate, std::size_t limit,
                         bool &exhausted);
  bool finalize_candidate(Candidate &candidate, bool early);
  bool search_anchors();
  void search_coarse_grid();
  void refine();
  void finalize();

  const TemplateState &state;
  const TemplateReference &definition;
  const PreparedTemplate &reference;
  PixelFrameView frame;
  NormalizedRegion available_region;
  std::span<const NormalizedRegion> unavailable_regions;
  std::uint32_t left;
  std::uint32_t top;
  std::uint32_t right;
  std::uint32_t bottom;
  TemplateMatchResult &result;
  std::span<const RenderedSize> sizes;
  std::vector<Candidate> candidates;
  std::size_t reference_start;
  std::size_t reference_budget;
  std::size_t final_candidate_cost;
  std::size_t search_budget;
  std::size_t coarse_limit;
  std::size_t refinement_limit;
  std::size_t early_validation_work{};
};

ReferenceSearchContext::ReferenceSearchContext(
    const ReferenceSearchInputs &inputs, TemplateMatchResult &result_value)
    : state(inputs.state), definition(inputs.definition),
      reference(inputs.reference), frame(inputs.frame),
      available_region(inputs.available_region),
      unavailable_regions(inputs.unavailable_regions), left(inputs.left),
      top(inputs.top), right(inputs.right), bottom(inputs.bottom),
      result(result_value), sizes(inputs.sizes),
      reference_start(result.pixel_comparisons),
      reference_budget(
          result.pixel_comparisons +
          (inputs.total_budget > result.pixel_comparisons
               ? inputs.total_budget - result.pixel_comparisons
               : 0U) /
              std::max<std::size_t>(1, inputs.references_remaining)),
      final_candidate_cost(reference.samples.size() * 2U),
      search_budget(calculate_search_budget()),
      coarse_limit(reference_start + search_budget * 3U / 4U),
      refinement_limit(reference_start + search_budget) {
  candidates.reserve(coarse_candidates);
}

std::size_t ReferenceSearchContext::calculate_search_budget() const {
  const auto allowance = reference_budget - result.pixel_comparisons;
  const auto reserve =
      std::min<std::size_t>(
          refinement_candidates,
          final_candidate_cost == 0 ? 0
                                    : allowance / final_candidate_cost) *
      final_candidate_cost;
  return reference_budget > result.pixel_comparisons + reserve
             ? reference_budget - result.pixel_comparisons - reserve
             : allowance / 2U;
}

void ReferenceSearchContext::keep_candidate(Candidate candidate) {
  if (candidates.size() < coarse_candidates) {
    candidates.push_back(std::move(candidate));
    return;
  }
  const auto weakest =
      std::ranges::min_element(candidates, {}, &Candidate::structure);
  if (weakest != candidates.end() && candidate.structure > weakest->structure)
    *weakest = std::move(candidate);
}

double ReferenceSearchContext::score_candidate(Candidate candidate,
                                               const std::size_t limit,
                                               bool &exhausted) {
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
  // locator_score fuses structure and color into one pixel pass, so this is
  // the complete coarse-stage work charge.
  result.pixel_comparisons += reference.locator_samples.size();
  ++result.candidates_evaluated;
  const double score = candidate.structure;
  keep_candidate(std::move(candidate));
  return score;
}

bool ReferenceSearchContext::finalize_candidate(Candidate &candidate,
                                                const bool early) {
  if (result.pixel_comparisons + final_candidate_cost > reference_budget)
    return false;
  candidate.structure = structure_score(
      reference.samples, reference.mean_luma, reference.luma_variance, frame,
      candidate.left, candidate.top, candidate.width, candidate.height);
  result.pixel_comparisons += reference.samples.size();
  const double coverage =
      color_coverage(reference, frame, candidate, state.color_tolerance);
  result.pixel_comparisons += reference.samples.size();
  if (early) early_validation_work += final_candidate_cost;
  const double confidence = 0.70 * candidate.structure + 0.30 * coverage;
  if (++result.candidates_scored == 1U || confidence > result.confidence) {
    result.confidence = confidence;
    result.reference_sha256 = definition.asset_sha256;
  }
  return true;
}

bool ReferenceSearchContext::search_anchors() {
  // Find an anchor color once and project every allowed size to an origin.
  const auto locator_cost = reference.locator_samples.size();
  const auto coarse_budget = search_budget * 3U / 4U;
  const auto scale_reserve =
      std::min<std::size_t>(coarse_budget, locator_cost * sizes.size());
  const auto anchor_budget = coarse_budget > scale_reserve
                                 ? (coarse_budget - scale_reserve) / 2U
                                 : 0U;
  const auto anchor_limit = reference_start + anchor_budget;
  const auto anchor_allowance =
      anchor_limit > result.pixel_comparisons
          ? anchor_limit - result.pixel_comparisons
          : 0U;
  const auto search_pixels =
      static_cast<std::uint64_t>(right - left) * (bottom - top);
  const auto anchor_step = std::max<std::uint32_t>(
      1, anchor_allowance == 0
             ? std::numeric_limits<std::uint32_t>::max()
             : static_cast<std::uint32_t>(std::ceil(std::sqrt(
                   static_cast<double>(search_pixels) /
                   static_cast<double>(anchor_allowance)))));
  const auto &anchor = reference.locator_samples.front();
  const auto stride = frame.stride();
  bool exhausted = anchor_allowance == 0;
  for (std::uint32_t y = top; y < bottom && !exhausted; y += anchor_step) {
    for (std::uint32_t x = left; x < right; x += anchor_step) {
      if (result.pixel_comparisons + 1U >
          anchor_limit + early_validation_work) {
        exhausted = true;
        break;
      }
      const auto pixel =
          frame.pixels[static_cast<std::size_t>(y) * stride + x];
      ++result.pixel_comparisons;
      if (!color_is_near(pixel, anchor.color, state.color_tolerance)) continue;
      for (const auto size : sizes) {
        const auto anchor_x = static_cast<std::uint32_t>(
            anchor.x * static_cast<float>(size.width));
        const auto anchor_y = static_cast<std::uint32_t>(
            anchor.y * static_cast<float>(size.height));
        const auto candidate_left = static_cast<std::int64_t>(x) - anchor_x;
        const auto candidate_top = static_cast<std::int64_t>(y) - anchor_y;
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
        const double score =
            score_candidate(candidate, anchor_limit, exhausted);
        if (score >= 0.90 && early_validation_work == 0U &&
            finalize_candidate(candidate, true) &&
            result.confidence >= state.confidence_threshold) {
          result.matched = true;
          return true;
        }
        if (exhausted) break;
      }
      if (exhausted) break;
    }
  }
  return false;
}

void ReferenceSearchContext::search_coarse_grid() {
  for (std::size_t size_index = 0; size_index < sizes.size(); ++size_index) {
    const auto size = sizes[size_index];
    const auto positions_x = right - left - size.width + 1U;
    const auto positions_y = bottom - top - size.height + 1U;
    const auto remaining =
        coarse_limit + early_validation_work > result.pixel_comparisons
            ? coarse_limit + early_validation_work - result.pixel_comparisons
            : 0U;
    if (remaining < reference.locator_samples.size()) break;
    const auto sizes_remaining = sizes.size() - size_index;
    const auto candidate_cap = std::max<std::size_t>(
        1, remaining /
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
                          .width = size.width,
                          .height = size.height,
                          .coarse_step = step};
      (void)score_candidate(std::move(candidate), coarse_limit, exhausted);
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
}

void ReferenceSearchContext::refine() {
  // Logarithmic hill search reaches pixel precision without a full scan.
  for (auto &candidate : candidates) {
    auto delta = candidate.coarse_step / 2U;
    while (delta > 0U) {
      Candidate best = candidate;
      for (const int y_direction : {-1, 0, 1}) {
        for (const int x_direction : {-1, 0, 1}) {
          if (x_direction == 0 && y_direction == 0) continue;
          const auto maximum_left = right - candidate.width;
          const auto maximum_top = bottom - candidate.height;
          const auto next_left = static_cast<std::uint32_t>(
              std::clamp<std::int64_t>(
                  static_cast<std::int64_t>(candidate.left) +
                      static_cast<std::int64_t>(x_direction) * delta,
                  left, maximum_left));
          const auto next_top = static_cast<std::uint32_t>(
              std::clamp<std::int64_t>(
                  static_cast<std::int64_t>(candidate.top) +
                      static_cast<std::int64_t>(y_direction) * delta,
                  top, maximum_top));
          if (next_left == candidate.left && next_top == candidate.top)
            continue;
          if (result.pixel_comparisons + reference.search_samples.size() >
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
}

void ReferenceSearchContext::finalize() {
  std::ranges::sort(candidates, std::greater{}, &Candidate::structure);
  std::size_t finalized{};
  for (auto &candidate : candidates) {
    if (finalized++ >= refinement_candidates) break;
    if (!finalize_candidate(candidate, false)) break;
  }
}

bool ReferenceSearchContext::run() {
  if (sizes.empty()) return false;
  if (search_anchors()) return true;
  search_coarse_grid();
  refine();
  finalize();
  return false;
}

} // namespace

struct CpuTemplateMatcher::Impl {
  // Every input rendered_sizes() reads; equal keys yield equal sizes.
  using SizesKey = std::tuple<double, double, double, std::uint32_t,
                              std::uint32_t, std::uint32_t, std::uint32_t>;
  static constexpr std::size_t max_cached_sizes = 64;

  // Prepared samples by verified content hash, then quality. Heterogeneous
  // lookup keeps the per-frame search allocation-free.
  std::unordered_map<std::string,
                      std::map<CaptureQuality, detail::TemplateMatcherReference>,
                      TransparentStringHash, std::equal_to<>>
      prepared;
  std::size_t preparation_count{};
  std::map<SizesKey, std::vector<RenderedSize>> sizes;

  const std::vector<RenderedSize> &
  rendered_sizes_for(const TemplateState &state, const PixelFrameView frame,
                     const std::uint32_t width, const std::uint32_t height) {
    const SizesKey key{state.expected_width, state.expected_height,
                       state.scale_tolerance, frame.width, frame.height,
                       width, height};
    if (const auto found = sizes.find(key); found != sizes.end())
      return found->second;
    if (sizes.size() >= max_cached_sizes) sizes.clear();
    return sizes.emplace(key, rendered_sizes(state, frame, width, height))
        .first->second;
  }
};

CpuTemplateMatcher::CpuTemplateMatcher() : impl_(std::make_unique<Impl>()) {}
CpuTemplateMatcher::~CpuTemplateMatcher() = default;
CpuTemplateMatcher::CpuTemplateMatcher(CpuTemplateMatcher &&) noexcept = default;
CpuTemplateMatcher &CpuTemplateMatcher::operator=(CpuTemplateMatcher &&) noexcept = default;

std::uint64_t CpuTemplateMatcher::prepared_cache_bytes() const noexcept {
  if (!impl_) return 0;
  std::uint64_t bytes{};
  for (const auto &[hash, qualities] : impl_->prepared) {
    bytes += hash.size();
    for (const auto &[quality, reference] : qualities) {
      (void)quality;
      bytes += (reference.samples.size() + reference.search_samples.size() +
                reference.locator_samples.size()) *
               sizeof(detail::TemplateMatcherSample);
    }
  }
  for (const auto &[key, sizes] : impl_->sizes) {
    (void)key;
    bytes += sizes.size() * sizeof(RenderedSize);
  }
  return bytes;
}

TemplateMatchResult CpuTemplateMatcher::evaluate(
    const TemplateState &state, const std::span<const TemplateImage> images,
    const PixelFrameView frame, const CaptureQuality quality,
    const NormalizedRegion available_region,
    const std::span<const NormalizedRegion> unavailable_regions) {
  if (!impl_) impl_ = std::make_unique<Impl>();
  TemplateMatchResult result;
  if (!frame.valid()) return result;
  const auto &region = state.search_region;
  const auto [left, top, right, bottom] =
      to_pixel_bounds(region, frame.width, frame.height);
  if (right <= left || bottom <= top) return result;
  const NormalizedRegion search_region{
      static_cast<double>(left) / frame.width,
      static_cast<double>(top) / frame.height,
      static_cast<double>(right - left) / frame.width,
      static_cast<double>(bottom - top) / frame.height};
  result.search_region_available =
      overlaps(search_region, available_region);

  const auto budget = comparison_budget(quality);
  const auto &sizes =
      impl_->rendered_sizes_for(state, frame, right - left, bottom - top);
  std::size_t reference_index{};
  for (const auto &definition : state.references) {
    ++reference_index;
    const auto image = std::ranges::find(
        images, definition.asset_sha256, &TemplateImage::asset_sha256);
    if (image == images.end() || !image->valid()) continue;
    auto by_hash = impl_->prepared.find(std::string_view(definition.asset_sha256));
    if (by_hash == impl_->prepared.end())
      by_hash = impl_->prepared.try_emplace(definition.asset_sha256).first;
    auto [prepared_it, inserted] = by_hash->second.try_emplace(quality);
    if (inserted) {
      ++impl_->preparation_count;
      // Runtimes hand in images they already compacted for this quality.
      prepared_it->second =
          detail::template_is_compact(*image, quality)
              ? detail::prepare_template(*image, quality)
              : detail::prepare_template(
                    compact_template_image(*image, quality), quality);
    }
    const auto &reference = prepared_it->second;
    if (reference.samples.empty() || reference.search_samples.empty() ||
        reference.locator_samples.empty())
      continue;

    ReferenceSearchContext search(
        {.state = state,
         .definition = definition,
         .reference = reference,
         .frame = frame,
         .available_region = available_region,
         .unavailable_regions = unavailable_regions,
         .left = left,
         .top = top,
         .right = right,
         .bottom = bottom,
         .sizes = sizes,
         .total_budget = budget,
         .references_remaining =
             state.references.size() - reference_index + 1U},
        result);
    if (search.run()) return result;
    if (result.pixel_comparisons >= budget) break;
  }
  // A zero threshold is legal, but only a scored candidate is match evidence.
  result.matched = result.candidates_scored != 0U &&
                   result.confidence >= state.confidence_threshold;
  return result;
}

void CpuTemplateMatcher::clear() {
  if (!impl_) return;
  impl_->prepared.clear();
  impl_->sizes.clear();
}

std::size_t CpuTemplateMatcher::preparation_count() const noexcept {
  return impl_ ? impl_->preparation_count : 0U;
}

void CpuTemplateMatcher::retain(const std::span<const std::string> asset_sha256) {
  if (!impl_) return;
  if (asset_sha256.empty()) {
    clear();
    return;
  }
  std::erase_if(impl_->prepared, [&](const auto &entry) {
    return std::ranges::find(asset_sha256, entry.first) == asset_sha256.end();
  });
}

} // namespace layerc::core
