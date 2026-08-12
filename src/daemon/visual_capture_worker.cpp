#include "visual_capture_worker.hpp"

#include "layerc/platform/visual_assets.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace layerc::daemon {
namespace {

struct PixelMeasurementKey {
  layerc::core::NormalizedRegion region;
  layerc::core::PixelPredicate predicate;
  layerc::core::CaptureQuality quality{};

  bool operator==(const PixelMeasurementKey &) const = default;
};

void hash_combine(std::size_t &seed, const std::size_t value) noexcept {
  seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

struct PixelMeasurementKeyHash {
  std::size_t operator()(const PixelMeasurementKey &key) const noexcept {
    std::size_t seed{};
    hash_combine(seed, std::hash<double>{}(key.region.x));
    hash_combine(seed, std::hash<double>{}(key.region.y));
    hash_combine(seed, std::hash<double>{}(key.region.width));
    hash_combine(seed, std::hash<double>{}(key.region.height));
    hash_combine(seed, static_cast<std::size_t>(key.quality));
    hash_combine(seed, key.predicate.index());
    std::visit(
        [&](const auto &predicate) {
          using T = std::decay_t<decltype(predicate)>;
          if constexpr (std::is_same_v<T,
                                       layerc::core::AverageColorPredicate>) {
            hash_combine(seed, predicate.target_color);
            hash_combine(seed, predicate.tolerance);
          } else if constexpr (
              std::is_same_v<T, layerc::core::BrightnessPredicate>) {
            hash_combine(seed, predicate.threshold);
            hash_combine(seed,
                         static_cast<std::size_t>(predicate.comparison));
          } else {
            hash_combine(seed, predicate.target_color);
            hash_combine(seed, predicate.tolerance);
            hash_combine(seed,
                         std::hash<double>{}(predicate.minimum_fraction));
          }
        },
        key.predicate);
    return seed;
  }
};

struct CachedPixelMeasurement {
  layerc::core::CaptureAvailability availability{};
  bool matched{};
};

using PixelMeasurementCache =
    std::unordered_map<PixelMeasurementKey, CachedPixelMeasurement,
                       PixelMeasurementKeyHash>;

std::pair<layerc::core::CaptureAvailability, bool> evaluate_once(
    const layerc::core::PixelRule &rule,
    const layerc::capture::CapturePollResult &capture,
    const layerc::core::CaptureQuality quality,
    PixelMeasurementCache &cache) {
  const PixelMeasurementKey key{.region = rule.region,
                                .predicate = rule.predicate,
                                .quality = quality};
  const auto cached = cache.find(key);
  if (cached != cache.end())
    return {cached->second.availability, cached->second.matched};

  auto availability = capture.availability;
  bool matched{};
  if (availability == layerc::core::CaptureAvailability::available &&
      capture.frame) {
    if (!layerc::capture::region_is_available(rule.region, *capture.frame)) {
      availability = layerc::core::CaptureAvailability::outside_output;
    } else {
      matched = layerc::core::evaluate_pixel_rule(
                    rule, capture.frame->view(), quality)
                    .matched;
    }
  }
  cache.emplace(key, CachedPixelMeasurement{.availability = availability,
                                             .matched = matched});
  return {availability, matched};
}

} // namespace

VisualCaptureWorker::VisualCaptureWorker(
    const HWND window, const layerc::core::CaptureQuality quality,
    const HANDLE snapshot_event)
    : window_(window), quality_(quality), snapshot_event_(snapshot_event) {}
VisualCaptureWorker::~VisualCaptureWorker() { stop(); }

void VisualCaptureWorker::start() {
  if (thread_.joinable()) return;
  stopping_.store(false, std::memory_order_release);
  thread_ = std::thread(&VisualCaptureWorker::run, this);
}

void VisualCaptureWorker::stop() {
  stopping_.store(true, std::memory_order_release);
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void VisualCaptureWorker::configure(
    std::vector<VisualEvaluationRequest> requests) {
  std::uint8_t maximum_rate{5};
  for (auto &request : requests) {
    request.sample_rate_hz = static_cast<std::uint8_t>(
        std::clamp<int>(request.sample_rate_hz, 5, 15));
    maximum_rate = std::max(maximum_rate, request.sample_rate_hz);
  }
  bool changed{};
  {
    const std::scoped_lock lock(mutex_);
    if (requests_ != requests) {
      requests_ = std::move(requests);
      ++request_revision_;
      latest_.evaluations.clear();
      sample_rate_hz_.store(maximum_rate, std::memory_order_release);
      changed = true;
    }
  }
  if (changed) wake_.notify_all();
}

void VisualCaptureWorker::set_excluded_screen_rects(
    const std::span<const RECT> rectangles) {
  bool changed{};
  {
    const std::scoped_lock lock(mutex_);
    changed = rectangles.size() != exclusions_.size() ||
              !std::equal(rectangles.begin(), rectangles.end(),
                          exclusions_.begin(), exclusions_.end(),
                          [](const RECT &left, const RECT &right) {
                            return left.left == right.left &&
                                   left.top == right.top &&
                                   left.right == right.right &&
                                   left.bottom == right.bottom;
                          });
    if (!changed) return;
    exclusions_.assign(rectangles.begin(), rectangles.end());
    ++exclusion_revision_;
  }
  if (changed) wake_.notify_all();
}

VisualCaptureSnapshot VisualCaptureWorker::snapshot() const {
  const std::scoped_lock lock(mutex_);
  return latest_;
}

void VisualCaptureWorker::run() {
  struct EvaluationState {
    layerc::core::TimePoint next_sample{};
    VisualEvaluationSnapshot snapshot;
  };

  layerc::capture::WindowCaptureSession capture(
      window_, {.allow_desktop_duplication = true, .quality = quality_});
  (void)capture.start();
  std::uint64_t applied_exclusion_revision =
      std::numeric_limits<std::uint64_t>::max();
  std::uint64_t applied_request_revision =
      std::numeric_limits<std::uint64_t>::max();
  std::uint64_t capture_sequence{};
  std::vector<VisualEvaluationRequest> requests;
  std::unordered_map<std::wstring, EvaluationState> evaluations;
  layerc::core::CpuTemplateMatcher template_matcher;
  while (!stopping_.load(std::memory_order_acquire)) {
    std::vector<RECT> exclusions;
    bool exclusions_changed{};
    bool requests_changed{};
    {
      const std::scoped_lock lock(mutex_);
      if (applied_exclusion_revision != exclusion_revision_) {
        exclusions = exclusions_;
        applied_exclusion_revision = exclusion_revision_;
        exclusions_changed = true;
      }
      if (applied_request_revision != request_revision_) {
        requests = requests_;
        applied_request_revision = request_revision_;
        evaluations.clear();
        requests_changed = true;
      }
    }
    if (exclusions_changed)
      capture.set_excluded_screen_rects(exclusions);
    if (requests_changed) {
      template_matcher.clear();
      struct ReferenceSet {
        std::shared_ptr<const std::vector<layerc::core::TemplateImage>> images;
        bool available{};
      };
      std::unordered_map<std::string, ReferenceSet> reference_sets;
      for (auto &request : requests) {
        for (auto &evaluation : request.templates) {
          std::string reference_key =
              std::to_string(static_cast<unsigned>(request.quality));
          for (const auto &definition : evaluation.state.references) {
            reference_key.push_back(':');
            reference_key += definition.asset_sha256;
          }
          if (const auto cached = reference_sets.find(reference_key);
              cached != reference_sets.end()) {
            evaluation.references = cached->second.images;
            evaluation.assets_available = cached->second.available;
            continue;
          }
          auto references =
              std::make_shared<std::vector<layerc::core::TemplateImage>>();
          references->reserve(evaluation.state.references.size());
          evaluation.assets_available = true;
          for (const auto &definition : evaluation.state.references) {
            auto loaded = layerc::platform::load_template_asset(
                definition.asset_sha256);
            auto *image =
                std::get_if<layerc::core::TemplateImage>(&loaded);
            if (image == nullptr) {
              evaluation.assets_available = false;
              references->clear();
              break;
            }
            references->push_back(layerc::core::compact_template_image(
                *image, request.quality));
          }
          evaluation.references = references;
          reference_sets.emplace(
              std::move(reference_key),
              ReferenceSet{.images = std::move(references),
                           .available = evaluation.assets_available});
        }
      }
    }

    const auto now = layerc::core::MonotonicClock::now();
    const bool foreground =
        GetAncestor(GetForegroundWindow(), GA_ROOT) == window_;
    auto result = capture.poll(now, foreground);
    ++capture_sequence;
    PixelMeasurementCache measurement_cache;
    measurement_cache.reserve(requests.size() *
                              layerc::core::max_pixel_rules);
    for (const auto &request : requests) {
      auto &state = evaluations[request.source_key];
      if (state.next_sample != layerc::core::TimePoint{} &&
          now < state.next_sample)
        continue;
      state.snapshot.source_key = request.source_key;
      state.snapshot.sequence = capture_sequence;
      state.snapshot.missing_template_assets = false;
      state.snapshot.measurements.clear();
      state.snapshot.measurements.reserve(request.rules.size() +
                                          request.templates.size());
      for (const auto &rule : request.rules) {
        const auto [availability, matched] = evaluate_once(
            rule, result, request.quality, measurement_cache);
        state.snapshot.measurements.push_back(
            {.observation_id = rule.id,
             .availability = availability,
             .matched = matched,
              .confidence = matched ? 1.0 : 0.0,
              .sampled_at = {}});
      }
      for (const auto &evaluation : request.templates) {
        auto availability = result.availability;
        bool matched{};
        double confidence{};
        if (!evaluation.assets_available || !evaluation.references ||
            evaluation.references->size() !=
                evaluation.state.references.size()) {
          availability = layerc::core::CaptureAvailability::unavailable;
          state.snapshot.missing_template_assets = true;
        } else if (availability ==
                       layerc::core::CaptureAvailability::available &&
                   result.frame) {
          const auto match = template_matcher.evaluate(
              evaluation.state, *evaluation.references,
              result.frame->view(), request.quality,
              result.frame->available_region,
              result.frame->unavailable_regions);
          if (!match.search_region_available) {
            availability =
                layerc::core::CaptureAvailability::outside_output;
          } else {
            matched = match.matched;
            confidence = match.confidence;
          }
        }
        state.snapshot.measurements.push_back(
            {.observation_id = evaluation.state.id,
             .availability = availability,
             .matched = matched,
             .confidence = confidence,
             .sampled_at = {}});
      }
      const auto evaluated_at = layerc::core::MonotonicClock::now();
      for (auto &measurement : state.snapshot.measurements)
        measurement.sampled_at = evaluated_at;
      state.next_sample =
          evaluated_at +
          std::chrono::milliseconds(1'000 / request.sample_rate_hz);
    }
    {
      const std::scoped_lock lock(mutex_);
      latest_.sequence = capture_sequence;
      latest_.result = std::move(result);
      latest_.gpu_description = capture.gpu_description();
      latest_.evaluations.clear();
      if (applied_request_revision == request_revision_) {
        latest_.evaluations.reserve(evaluations.size());
        for (const auto &[key, state] : evaluations) {
          (void)key;
          if (state.snapshot.sequence != 0)
            latest_.evaluations.push_back(state.snapshot);
        }
      }
    }
    if (snapshot_event_ != nullptr) SetEvent(snapshot_event_);

    const auto rate = std::max<std::uint8_t>(
        1, sample_rate_hz_.load(std::memory_order_acquire));
    const auto interval = std::chrono::milliseconds(1'000 / rate);
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, interval, [&] {
      return stopping_.load(std::memory_order_acquire) ||
             request_revision_ != applied_request_revision ||
             exclusion_revision_ != applied_exclusion_revision;
    });
  }
  capture.stop();
}

} // namespace layerc::daemon
