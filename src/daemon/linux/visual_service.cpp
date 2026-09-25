#include "visual_service.hpp"

#include "layerc/capture/visual_evaluation.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <ranges>
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
    const layerc::runtime::CapabilityDemand &demand) {
  struct Desired {
    CaptureQuality quality{CaptureQuality::low};
    std::vector<const InputSession *> sessions;
  };
  std::map<std::uint64_t, Desired> desired;
  for (const auto &consumer : demand.visual_consumers()) {
    if (consumer.runtime != layerc::core::VisualRuntime::inline_process) continue;
    const auto found = std::ranges::find_if(sessions, [&](const auto &session) {
      return session.window == consumer.window.opaque &&
             session.profile.id == consumer.profile_id;
    });
    if (found == sessions.end() || !found->visual_profile) continue;
    auto &entry = desired[found->window];
    entry.quality = std::max(entry.quality, found->visual_profile->quality);
    entry.sessions.push_back(&*found);
  }

  asset_diagnostics_.clear();
  capture_diagnostics_.clear();
  fault_.clear();
  template_matcher_.clear();
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

    for (const auto *session : entry.sessions) {
      Evaluation evaluation;
      evaluation.source_path = session->source_path;
      evaluation.profile_id = session->profile.id;
      evaluation.profile = *session->visual_profile;
      for (const auto &state : evaluation.profile.templates) {
        if (!uses_template(evaluation.profile, state.id)) continue;
        TemplateEvaluation loaded;
        loaded.state = state;
        loaded.assets_available = true;
        for (const auto &reference : state.references) {
          auto asset = layerc::platform::load_profile_template_asset(
              session->source_path, reference.asset_sha256);
          auto *image = std::get_if<layerc::core::TemplateImage>(&asset);
          if (image == nullptr) {
            loaded.assets_available = false;
            const auto &error = std::get<VisualAssetError>(asset);
            append_unique(asset_diagnostics_,
                          session->source_path.string() + ": " + error.message);
            loaded.references.clear();
            break;
          }
          loaded.references.push_back(layerc::core::compact_template_image(
              *image, evaluation.profile.quality));
        }
        evaluation.templates.push_back(std::move(loaded));
      }
      target.evaluations.push_back(std::move(evaluation));
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
  const auto due = [now](const auto &evaluation) {
    return evaluation.next_sample == TimePoint{} ||
           now >= evaluation.next_sample;
  };
  for (auto &target : targets_) {
    if (!std::ranges::any_of(target.evaluations, due) || !target.capture)
      continue;

    auto capture = target.capture->poll(now).result;
    if (!capture.diagnostic.empty())
      append_unique(capture_diagnostics_, capture.diagnostic);
    for (auto &evaluation : target.evaluations) {
      if (!due(evaluation)) continue;
      std::vector<ObservationMeasurement> measurements;
      measurements.reserve(evaluation.profile.rules.size() +
                           evaluation.templates.size());
      // Shared synchronous evaluator (F9): ROI/error-to-measurement policy
      // lives in capture/visual_evaluation.hpp, not in this host loop.
      // sampled_at is the capture poll time, fixed before evaluation.
      for (const auto &rule : evaluation.profile.rules)
        measurements.push_back(layerc::capture::evaluate_pixel_measurement(
            rule, {capture, evaluation.profile.quality, now}));
      for (const auto &item : evaluation.templates)
        measurements.push_back(layerc::capture::evaluate_template_measurement(
            item.state, item.references, item.assets_available,
            {capture, evaluation.profile.quality, now}, template_matcher_));
      if (auto *session = find_session(sessions, evaluation.source_path,
                                       target.window)) {
        auto accepted = session->feed_measurements(measurements, now);
        triggered.insert(triggered.end(),
                         std::make_move_iterator(accepted.begin()),
                         std::make_move_iterator(accepted.end()));
      }
      const auto rate = std::max<std::uint8_t>(1U,
                                               evaluation.profile.sample_rate_hz);
      evaluation.next_sample =
          now + std::chrono::milliseconds(1'000 / rate);
    }
  }
  return triggered;
}

std::optional<TimePoint>
X11VisualService::next_deadline(const TimePoint now) const noexcept {
  std::optional<TimePoint> result;
  for (const auto &target : targets_)
    for (const auto &evaluation : target.evaluations) {
      const auto due = evaluation.next_sample == TimePoint{}
                           ? now
                           : evaluation.next_sample;
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
  template_matcher_.clear();
  asset_diagnostics_.clear();
  capture_diagnostics_.clear();
  fault_.clear();
}

layerc::runtime::RuntimeResourceStats
X11VisualService::resources() const noexcept {
  layerc::runtime::RuntimeResourceStats result;
  for (const auto &target : targets_) {
    ++result.objects;
    if (target.capture) result += target.capture->resources();
    for (const auto &evaluation : target.evaluations)
      for (const auto &item : evaluation.templates)
        for (const auto &image : item.references) {
          ++result.decoded_assets;
          result.cache_bytes +=
              image.pixels.size() * sizeof(layerc::core::RgbPixel) +
              image.alpha.size();
        }
  }
  result.timers = targets_.empty() ? 0U : 1U;
  return result;
}

} // namespace layerc::daemon
