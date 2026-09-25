#include "visual_capture_worker.hpp"

#include "layerc/capture/visual_evaluation.hpp"
#include "layerc/platform/visual_assets_windows.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
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

struct ReferenceSetKey {
  std::vector<std::string> hashes;
  layerc::core::CaptureQuality quality{};

  bool operator==(const ReferenceSetKey &) const = default;
};

struct ReferenceSetKeyHash {
  std::size_t operator()(const ReferenceSetKey &key) const noexcept {
    std::size_t seed = static_cast<std::size_t>(key.quality);
    for (const auto &hash : key.hashes)
      hash_combine(seed, std::hash<std::string>{}(hash));
    return seed;
  }
};

struct ReferenceSet {
  std::shared_ptr<const std::vector<layerc::core::TemplateImage>> images;
  bool available{};
};

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

  const auto measurement = layerc::capture::evaluate_pixel_measurement(
      rule, {capture, quality, layerc::core::TimePoint{}});
  const auto availability = measurement.availability;
  const auto matched = measurement.matched;
  cache.emplace(key, CachedPixelMeasurement{.availability = availability,
                                             .matched = matched});
  return {availability, matched};
}

} // namespace

VisualCaptureWorker::VisualCaptureWorker(
    const HWND window, const HANDLE snapshot_event)
    : window_(window), snapshot_event_(snapshot_event) {}
VisualCaptureWorker::~VisualCaptureWorker() { stop(); }

void VisualCaptureWorker::start() {
  if (thread_.joinable()) return;
  stopping_.store(false, std::memory_order_release);
  thread_ = std::thread(&VisualCaptureWorker::run, this);
}

void VisualCaptureWorker::stop() noexcept {
  stopping_.store(true, std::memory_order_release);
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void VisualCaptureWorker::configure(
    std::vector<VisualEvaluationRequest> requests,
    const layerc::core::CaptureQuality quality) {
  std::uint8_t maximum_rate{5};
  for (auto &request : requests) {
    request.sample_rate_hz = static_cast<std::uint8_t>(
        std::clamp<int>(request.sample_rate_hz, 5, 15));
    maximum_rate = std::max(maximum_rate, request.sample_rate_hz);
  }
  bool changed{};
  {
    const std::scoped_lock lock(mutex_);
    if (requests_ != requests ||
        quality_.load(std::memory_order_relaxed) != quality) {
      requests_ = std::move(requests);
      quality_.store(quality, std::memory_order_relaxed);
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
  {
    const std::scoped_lock lock(mutex_);
    if (same_rectangles(rectangles, exclusions_)) return;
    exclusions_.assign(rectangles.begin(), rectangles.end());
    ++exclusion_revision_;
  }
  wake_.notify_all();
}

VisualCaptureSnapshot VisualCaptureWorker::snapshot() const {
  const std::scoped_lock lock(mutex_);
  return latest_;
}

VisualCaptureHostStatus VisualCaptureWorker::status() const {
  return {.running = thread_.joinable() &&
                     !stopping_.load(std::memory_order_acquire),
          .quarantined = false,
          .fault_count = 0,
          .helper_process_id = 0,
          .last_error = {}};
}

struct VisualCaptureWorker::RunState {
  struct EvaluationState {
    layerc::core::TimePoint next_sample{};
    VisualEvaluationSnapshot snapshot;
  };

  RunState(const HWND window, const layerc::core::CaptureQuality quality)
      : applied_quality(quality),
        capture(window,
                {.allow_desktop_duplication = true, .quality = quality}) {}

  layerc::core::CaptureQuality applied_quality;
  layerc::capture::WindowCaptureSession capture;
  std::uint64_t applied_exclusion_revision =
      std::numeric_limits<std::uint64_t>::max();
  std::uint64_t applied_request_revision =
      std::numeric_limits<std::uint64_t>::max();
  std::uint64_t capture_sequence{};
  std::vector<VisualEvaluationRequest> requests;
  std::unordered_map<std::string, EvaluationState> evaluations;
  layerc::core::CpuTemplateMatcher template_matcher;
};

void VisualCaptureWorker::apply_pending_configuration(RunState &state) {
  std::vector<RECT> exclusions;
  bool exclusions_changed{};
  bool requests_changed{};
  auto requested_quality = state.applied_quality;
  {
    const std::scoped_lock lock(mutex_);
    if (state.applied_exclusion_revision != exclusion_revision_) {
      exclusions = exclusions_;
      state.applied_exclusion_revision = exclusion_revision_;
      exclusions_changed = true;
    }
    if (state.applied_request_revision != request_revision_) {
      state.requests = requests_;
      requested_quality = quality_.load(std::memory_order_relaxed);
      state.applied_request_revision = request_revision_;
      state.evaluations.clear();
      requests_changed = true;
    }
  }
  if (exclusions_changed)
    state.capture.set_excluded_screen_rects(exclusions);
  if (!requests_changed)
    return;
  if (requested_quality != state.applied_quality) {
    state.capture.set_quality(requested_quality);
    state.applied_quality = requested_quality;
  }
  state.template_matcher.clear();
  std::unordered_map<ReferenceSetKey, ReferenceSet, ReferenceSetKeyHash>
      reference_sets;
  for (auto &request : state.requests) {
    for (auto &evaluation : request.templates) {
      ReferenceSetKey reference_key{.hashes = {}, .quality = request.quality};
      reference_key.hashes.reserve(evaluation.state.references.size());
      for (const auto &definition : evaluation.state.references)
        reference_key.hashes.push_back(definition.asset_sha256);
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
        auto loaded =
            layerc::platform::load_template_asset(definition.asset_sha256);
        auto *image = std::get_if<layerc::core::TemplateImage>(&loaded);
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

void VisualCaptureWorker::evaluate_requests(
    RunState &run, const layerc::capture::CapturePollResult &result,
    const layerc::core::TimePoint now) {
  PixelMeasurementCache measurement_cache;
  measurement_cache.reserve(run.requests.size() *
                            layerc::core::max_pixel_rules);
  for (const auto &request : run.requests) {
    auto &state = run.evaluations[request.source_key];
    if (state.next_sample != layerc::core::TimePoint{} &&
        now < state.next_sample)
      continue;
    state.snapshot.source_key = request.source_key;
    state.snapshot.sequence = run.capture_sequence;
    state.snapshot.missing_template_assets = false;
    state.snapshot.measurements.clear();
    state.snapshot.measurements.reserve(request.rules.size() +
                                        request.templates.size());
    for (const auto &rule : request.rules) {
      const auto [availability, matched] =
          evaluate_once(rule, result, request.quality, measurement_cache);
      state.snapshot.measurements.push_back(
          {.observation_id = rule.id,
           .availability = availability,
           .matched = matched,
           .confidence = matched ? 1.0 : 0.0,
           .sampled_at = now});
    }
    for (const auto &evaluation : request.templates) {
      const std::span<const layerc::core::TemplateImage> references =
          evaluation.references
              ? std::span{evaluation.references->data(),
                          evaluation.references->size()}
              : std::span<const layerc::core::TemplateImage>{};
      state.snapshot.measurements.push_back(
          layerc::capture::evaluate_template_measurement(
              evaluation.state, references, evaluation.assets_available,
              {result, request.quality, now}, run.template_matcher));
      if (!evaluation.assets_available || !evaluation.references ||
          evaluation.references->size() != evaluation.state.references.size())
        state.snapshot.missing_template_assets = true;
    }
    state.next_sample =
        now + std::chrono::milliseconds(1'000 / request.sample_rate_hz);
  }
}

void VisualCaptureWorker::publish_latest(
    const RunState &state, layerc::capture::CapturePollResult result) {
  {
    const std::scoped_lock lock(mutex_);
    latest_.sequence = state.capture_sequence;
    latest_.result = std::move(result);
    latest_.gpu_description = state.capture.gpu_description();
    latest_.evaluations.clear();
    if (state.applied_request_revision == request_revision_) {
      latest_.evaluations.reserve(state.evaluations.size());
      for (const auto &[key, evaluation] : state.evaluations) {
        (void)key;
        if (evaluation.snapshot.sequence != 0)
          latest_.evaluations.push_back(evaluation.snapshot);
      }
    }
  }
  if (snapshot_event_ != nullptr)
    SetEvent(snapshot_event_);
}

void VisualCaptureWorker::run() {
  RunState state(window_, quality_.load(std::memory_order_acquire));
  (void)state.capture.start();
  while (!stopping_.load(std::memory_order_acquire)) {
    apply_pending_configuration(state);
    const auto now = layerc::core::MonotonicClock::now();
    const bool foreground =
        GetAncestor(GetForegroundWindow(), GA_ROOT) == window_;
    auto result = state.capture.poll(now, foreground).result;
    ++state.capture_sequence;
    evaluate_requests(state, result, now);
    publish_latest(state, std::move(result));

    const auto rate = std::max<std::uint8_t>(
        1, sample_rate_hz_.load(std::memory_order_acquire));
    const auto interval = std::chrono::milliseconds(1'000 / rate);
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, interval, [&] {
      return stopping_.load(std::memory_order_acquire) ||
             request_revision_ != state.applied_request_revision ||
             exclusion_revision_ != state.applied_exclusion_revision;
    });
  }
  state.capture.stop();
}

} // namespace layerc::daemon
