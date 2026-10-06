#include "visual_service.hpp"

#include "layerc/capture/visual_evaluation.hpp"
#include "layerc/core/hash.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <ranges>
#include <set>
#include <utility>

namespace layerc::daemon {
namespace {

using layerc::core::CaptureAvailability;
using layerc::core::CaptureQuality;
using layerc::core::ObservationMeasurement;
using layerc::core::TimePoint;
using layerc::platform::VisualAssetError;

[[nodiscard]] InputSession *find_session(
    const std::span<InputSession> sessions, const std::filesystem::path &path,
    const std::uint64_t window) noexcept {
  for (auto &session : sessions)
    if (session.window == window && session.source_path == path) return &session;
  return nullptr;
}

[[nodiscard]] bool uses_template(const layerc::core::VisualProfile &profile,
                                 const std::string_view id) noexcept {
  return std::ranges::any_of(profile.triggers, [&](const auto &trigger) {
    return trigger.enabled && trigger.observation_id == id;
  });
}

void append_unique(std::vector<std::string> &values, std::string value) {
  if (value.empty() || std::ranges::find(values, value) != values.end()) return;
  values.push_back(std::move(value));
}

} // namespace

void X11VisualService::attach(layerc::platform::X11Display &display) noexcept {
  display_ = &display;
}

void X11VisualService::refresh(
    const std::span<InputSession> sessions,
    const layerc::runtime::CapabilityDemand &demand,
    const bool configuration_changed,
    std::optional<std::span<const VisualConfiguration>> configurations) {
  struct Desired {
    CaptureQuality quality{CaptureQuality::low};
    std::vector<const InputSession *> sessions;
  };
  std::map<std::uint64_t, Desired> desired;
  for (const auto &consumer : demand.visual_consumers()) {
    if (consumer.runtime != layerc::core::VisualRuntime::inline_process) continue;
    const auto found = std::ranges::find_if(sessions, [&](const auto &session) {
      return session.window == consumer.window.opaque &&
             session.automation.profile().id == consumer.profile_id;
    });
    if (found == sessions.end() || !found->visual_profile) continue;
    auto &entry = desired[found->window];
    entry.quality = std::max(entry.quality, found->visual_profile->quality);
    entry.sessions.push_back(&*found);
  }

  asset_diagnostics_.clear();
  capture_diagnostics_.clear();
  fault_.clear();
  const auto stamps_for = [this](const std::filesystem::path &source_path,
                                 const layerc::core::VisualProfile &profile) {
    std::vector<AssetStamp> stamps;
    for (const auto &state : layerc::core::observations_of<layerc::core::TemplateState>(profile.observations)) {
      if (!uses_template(profile, state.id)) continue;
      for (const auto &reference : state.references) {
        AssetStamp stamp;
        stamp.sha256 = reference.asset_sha256;
        const auto path = layerc::platform::profile_visual_asset_path(
            source_path, stamp.sha256);
        ++asset_checks_;
        if (const auto bytes = layerc::platform::read_template_asset_bytes(path))
          stamp.contents_sha256 =
              layerc::core::sha256(std::as_bytes(std::span(*bytes)));
        stamps.push_back(std::move(stamp));
      }
    }
    return stamps;
  };

  // The daemon supplies its loaded configuration, including profiles with no
  // current window. Standalone adapter tests can infer it from their sessions.
  std::vector<VisualConfiguration> inferred;
  if (!configurations) {
    for (const auto &session : sessions)
      if (session.visual_profile &&
          session.automation.profile().visual_runtime ==
              layerc::core::VisualRuntime::inline_process)
        inferred.push_back({session.source_path, session.automation.profile().id,
                            *session.visual_profile});
    configurations = std::span<const VisualConfiguration>{inferred};
  }
  // Only a store reload hashes configured assets. Window/device scans rematch
  // retained evaluations without reading files.
  bool evaluations_changed = false;
  std::set<std::filesystem::path> configured_paths;
  std::set<std::filesystem::path> matched_paths;
  for (const auto &session : sessions) {
    if (!session.visual_profile ||
        session.automation.profile().visual_runtime !=
            layerc::core::VisualRuntime::inline_process)
      continue;
    matched_paths.insert(session.source_path);
  }
  for (const auto &configuration : *configurations) {
    const auto &source_path = configuration.source_path;
    const auto &profile = configuration.profile;
    configured_paths.insert(source_path);
    const auto previous = evaluations_.find(source_path);
    const bool same_profile = previous != evaluations_.end() &&
                              previous->second.profile_id ==
                                  configuration.profile_id &&
                              previous->second.profile == profile;
    if (same_profile && !configuration_changed) continue;
    auto stamps = stamps_for(source_path, profile);
    if (same_profile && previous->second.asset_stamps == stamps) continue;

    Evaluation evaluation;
    evaluation.source_path = source_path;
    evaluation.profile_id = configuration.profile_id;
    evaluation.profile = profile;
    evaluation.asset_stamps = std::move(stamps);
    for (const auto &state :
         layerc::core::observations_of<layerc::core::TemplateState>(evaluation.profile.observations)) {
      if (!uses_template(evaluation.profile, state.id)) continue;
      TemplateEvaluation loaded;
      loaded.state = state;
      loaded.assets_available = true;
      for (const auto &reference : state.references) {
        ++asset_loads_;
        auto asset = layerc::platform::load_profile_template_asset(
            source_path, reference.asset_sha256);
        auto *image = std::get_if<layerc::core::TemplateImage>(&asset);
        if (image == nullptr) {
          loaded.assets_available = false;
          const auto &error = std::get<VisualAssetError>(asset);
          append_unique(evaluation.diagnostics,
                        source_path.string() + ": " + error.message);
          loaded.references.clear();
          break;
        }
        loaded.references.push_back(layerc::core::compact_template_image(
            *image, evaluation.profile.quality));
      }
      evaluation.templates.push_back(std::move(loaded));
    }
    ++evaluation_builds_;
    evaluations_.insert_or_assign(source_path, std::move(evaluation));
    evaluations_changed = true;
  }
  std::size_t erased{};
  if (configuration_changed)
    erased = std::erase_if(evaluations_, [&](const auto &entry) {
      return !configured_paths.contains(entry.first);
    });
  evaluations_changed = evaluations_changed || erased != 0;
  std::vector<std::string> retained_hashes;
  for (const auto &[path, evaluation] : evaluations_) {
    for (const auto &item : evaluation.templates)
      for (const auto &image : item.references)
        retained_hashes.push_back(image.asset_sha256);
    if (matched_paths.contains(path))
      for (const auto &diagnostic : evaluation.diagnostics)
        append_unique(asset_diagnostics_, diagnostic);
  }
  if (evaluations_changed) template_matcher_.retain(retained_hashes);

  std::vector<Target> next;
  next.reserve(desired.size());
  for (const auto &[window, entry] : desired) {
    Target target;
    target.window = window;
    target.quality = entry.quality;
    if (const auto previous = std::ranges::find(targets_, window, &Target::window);
        previous != targets_.end()) {
      target.capture = std::move(previous->capture);
      if (target.capture) target.capture->set_quality(target.quality);
    }
    if (!target.capture && display_ != nullptr)
      target.capture = std::make_unique<layerc::platform::X11CaptureSession>(
          *display_, layerc::runtime::WindowId{window}, target.quality);

    for (const auto *session : entry.sessions)
      if (const auto found = evaluations_.find(session->source_path);
          found != evaluations_.end()) {
        auto &evaluation = found->second;
        if (evaluation.last_target_window != 0 &&
            evaluation.last_target_window != window)
          evaluation.next_sample = {};
        evaluation.last_target_window = window;
        target.evaluations.push_back(&found->second);
      }
    next.push_back(std::move(target));
  }
  targets_ = std::move(next);
}

std::vector<std::string>
X11VisualService::poll(const std::span<InputSession> sessions,
                       const TimePoint now) {
  std::vector<std::string> triggered;
  capture_diagnostics_.clear();
  const auto due = [now](const Evaluation *evaluation) {
    return evaluation->next_sample == TimePoint{} ||
           now >= evaluation->next_sample;
  };
  for (auto &target : targets_) {
    if (!std::ranges::any_of(target.evaluations, due) || !target.capture)
      continue;

    auto capture = target.capture->poll(now).result;
    if (!capture.diagnostic.empty())
      append_unique(capture_diagnostics_, capture.diagnostic);
    for (auto *evaluation : target.evaluations) {
      if (!due(evaluation)) continue;
      std::vector<ObservationMeasurement> measurements;
      measurements.reserve(evaluation->profile.observations.size());
      // Shared synchronous evaluator (F9): ROI/error-to-measurement policy
      // lives in capture/visual_evaluation.hpp, not in this host loop.
      // sampled_at is the capture poll time, fixed before evaluation.
      for (const auto &rule : layerc::core::observations_of<layerc::core::PixelRule>(
               evaluation->profile.observations))
        measurements.push_back(layerc::capture::evaluate_pixel_measurement(
            rule, {capture, evaluation->profile.quality, now}));
      for (const auto &item : evaluation->templates)
        measurements.push_back(layerc::capture::evaluate_template_measurement(
            item.state, item.references, item.assets_available,
            {capture, evaluation->profile.quality, now}, template_matcher_));
      if (auto *session = find_session(sessions, evaluation->source_path,
                                       target.window)) {
        auto accepted = session->feed_measurements(measurements, now);
        triggered.insert(triggered.end(),
                         std::make_move_iterator(accepted.begin()),
                         std::make_move_iterator(accepted.end()));
      }
      const auto rate = std::max<std::uint8_t>(1U,
                                               evaluation->profile.sample_rate_hz);
      evaluation->next_sample =
          now + std::chrono::milliseconds(1'000 / rate);
    }
  }
  return triggered;
}

std::optional<TimePoint>
X11VisualService::next_deadline(const TimePoint now) const noexcept {
  std::optional<TimePoint> result;
  for (const auto &target : targets_)
    for (const auto *evaluation : target.evaluations) {
      const auto due = evaluation->next_sample == TimePoint{}
                           ? now
                           : evaluation->next_sample;
      if (!result.has_value() || due < *result) result = due;
    }
  return result;
}

std::vector<std::string> X11VisualService::unavailable() const {
  std::vector<std::string> result = asset_diagnostics_;
  for (const auto &diagnostic : capture_diagnostics_)
    append_unique(result, diagnostic);
  append_unique(result, fault_);
  return result;
}

layerc::runtime::ServiceReconcileResult
X11VisualService::reconcile(
    const layerc::runtime::CapabilityDemand &demand) {
  if (!demand.required(layerc::runtime::Capability::visual))
    return {false, "visual: service invoked without demand"};
  if (display_ == nullptr || !display_->valid())
    return {false, "visual: no X display for capture"};
  fault_.clear();
  for (auto &target : targets_) {
    if (target.capture && target.capture->start()) continue;
    append_unique(capture_diagnostics_,
                  "visual: X11 capture could not start for window " +
                      std::to_string(target.window));
  }
  return {};
}

void X11VisualService::quiesce() noexcept {
  for (auto &target : targets_)
    if (target.capture) target.capture->stop();
  targets_.clear();
  capture_diagnostics_.clear();
  fault_.clear();
}

layerc::runtime::RuntimeResourceStats
X11VisualService::resources() const noexcept {
  layerc::runtime::RuntimeResourceStats result;
  for (const auto &target : targets_) {
    ++result.objects;
    if (target.capture) result += target.capture->resources();
  }
  for (const auto &[path, evaluation] : evaluations_) {
    (void)path;
    for (const auto &item : evaluation.templates)
      for (const auto &image : item.references) {
        ++result.decoded_assets;
        result.cache_bytes +=
            image.pixels.size() * sizeof(layerc::core::RgbPixel) +
            image.alpha.size();
      }
  }
  result.cache_bytes += template_matcher_.prepared_cache_bytes();
  result.timers = targets_.empty() ? 0U : 1U;
  return result;
}

X11VisualService::WorkStats X11VisualService::work_stats() const noexcept {
  return {.asset_checks = asset_checks_,
          .asset_loads = asset_loads_,
          .evaluation_builds = evaluation_builds_,
          .template_preparations = template_matcher_.preparation_count()};
}

} // namespace layerc::daemon
