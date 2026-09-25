// X11 input capability (V2.6.1): profile sessions driven by passive XI2 raw
// input, registered with the daemon kernel as Capability::input.
#include "input_service.hpp"

#include "layerc/core/automation_session.hpp"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <utility>

namespace layerc::daemon {
namespace {

using layerc::core::AutomationEffect;
using layerc::core::AutomationEventKind;
using layerc::core::AutomationPersistentValues;
using layerc::core::AutomationProfile;
using layerc::core::TimePoint;
using layerc::core::VisualProfile;
using layerc::core::VisualTriggerRequest;

struct LoadedVisual {
  std::optional<VisualProfile> profile;
  std::string diagnostic;
  bool invalid{};
};

struct LoadedAutomation {
  std::optional<AutomationProfile> profile;
  AutomationPersistentValues state;
  std::string diagnostic;
};

[[nodiscard]] LoadedVisual
load_visual(const layerc::platform::StoredProfile &source) {
  LoadedVisual result;
  auto sidecar =
      layerc::platform::load_visual_sidecar(source.path, source.profile);
  if (!sidecar.profile.has_value()) {
    if (sidecar.state == layerc::platform::VisualSidecarState::invalid) {
      result.invalid = true;
      result.diagnostic = source.path.string() + ": " + sidecar.error.message;
    }
    return result;
  }
  if (layerc::core::has_enabled_visual_assignments(*sidecar.profile))
    result.profile = std::move(sidecar.profile);
  return result;
}

[[nodiscard]] bool has_enabled_rules(const AutomationProfile &profile) {
  return std::ranges::any_of(profile.automations, [](const auto &automation) {
    return automation.enabled && !automation.rules.empty();
  });
}

[[nodiscard]] LoadedAutomation
load_automation(const layerc::platform::StoredProfile &source) {
  LoadedAutomation result;
  auto sidecar = layerc::platform::load_automation_sidecar(source.path,
                                                           source.profile);
  if (!sidecar.profile.has_value()) {
    if (sidecar.state == layerc::platform::AutomationSidecarState::invalid)
      result.diagnostic = source.path.string() + ": " + sidecar.error.message;
    return result;
  }

  auto state = layerc::platform::load_automation_state(source.path,
                                                       *sidecar.profile);
  result.state = std::move(state.values);
  if (state.error.has_value())
    result.diagnostic = source.path.string() + ": " + state.error->message;
  if (has_enabled_rules(*sidecar.profile))
    result.profile = std::move(sidecar.profile);
  return result;
}

[[nodiscard]] bool
same_visual(const InputSession &session,
            const std::optional<VisualProfile> &profile) noexcept {
  return session.visual_profile == profile;
}

[[nodiscard]] bool same_automation(
    const InputSession &session,
    const std::optional<AutomationProfile> &profile) noexcept {
  if (!profile.has_value()) return session.automation_runtime == nullptr;
  return session.automation_runtime != nullptr &&
         session.automation_runtime->profile() == *profile;
}

} // namespace

// The adapter supplies only durable storage and fault reporting. All event,
// validation, cooldown and origin policy lives in core::AutomationSession.
namespace {
class InputSessionSink final : public layerc::core::AutomationSessionSink {
public:
  explicit InputSessionSink(InputSession &session) : session_(session) {}
  void on_result(const layerc::core::AutomationEvaluationResult &result) override {
    if (result.error)
      session_.automation_faults.push_back(result.error->path + ": " +
                                           result.error->message);
    for (const auto &id : result.faulted_automations)
      session_.automation_faults.push_back("automation fault: " + id);
  }
  void on_persistent_change(const AutomationPersistentValues &,
                            const TimePoint now) override {
    layerc::core::automation_checkpoint_arm(session_.automation_state_due, now);
  }
  void on_stopping(const TimePoint now) override {
    session_.checkpoint_automation_now(now);
  }
private:
  InputSession &session_;
};
} // namespace

InputSession::InputSession(
    layerc::platform::StoredProfile source, const std::uint64_t window,
    const bool global, std::optional<VisualProfile> loaded_visual_profile,
    std::optional<AutomationProfile> automation_profile,
    AutomationPersistentValues automation_state, const bool invalid_visual_sidecar)
    : layerc::core::AutomationSession(source.profile),
      source_path(std::move(source.path)), profile(std::move(source.profile)),
      window(window), global(global), matcher(profile.bindings),
      visual_profile(std::move(loaded_visual_profile)) {
  set_invalid_visual_sidecar(invalid_visual_sidecar);
  if (visual_profile)
    visual_triggers =
        std::make_unique<layerc::core::VisualTriggerEngine>(*visual_profile);
  if (automation_profile) {
    InputSessionSink sink(*this);
    (void)start(std::move(*automation_profile), std::move(automation_state),
                profile, layerc::core::MonotonicClock::now(), &sink);
  }
}

void InputSession::checkpoint_automation_state(const TimePoint now,
                                               const bool force) {
  if (automation_runtime && automation_runtime->persistent_dirty())
    layerc::core::automation_checkpoint_arm(automation_state_due, now);
  if (!automation_runtime || !layerc::core::automation_checkpoint_ready(
                                 automation_state_due, now, force)) return;
  const auto written = layerc::platform::write_automation_state(
      source_path, automation_runtime->profile(),
      automation_runtime->snapshot().persistent_values);
  if (const auto error = written.error(); error.has_value())
    automation_faults.push_back(
        std::string("automation checkpoint failed (") +
        layerc::platform::profile_write_error_text(*error) +
        "): " + source_path.string());
  else
    automation_runtime->mark_persistent_saved();
  layerc::core::automation_checkpoint_report(automation_state_due, now,
                                             written);
}

std::vector<std::string>
InputSession::feed(const layerc::core::InputEvent &input, const TimePoint now) {
  InputSessionSink sink(*this);
  return feed_input(matcher, input, profile, now, &sink).triggered;
}

std::vector<std::string> InputSession::feed_measurements(
    const std::span<const layerc::core::ObservationMeasurement> measurements,
    const TimePoint now) {
  InputSessionSink sink(*this);
  return layerc::core::AutomationSession::feed_measurements(
      measurements, profile, now, &sink).triggered;
}

void InputSession::poll_automation(const TimePoint now) {
  InputSessionSink sink(*this);
  (void)poll(profile, now, &sink);
  checkpoint_automation_state(now, false);
}

std::optional<TimePoint>
InputSession::next_automation_deadline(const TimePoint now) {
  return next_deadline(now, automation_state_due);
}

void InputSession::stop_automation(const TimePoint now) {
  InputSessionSink sink(*this);
  (void)stop(profile, now, &sink);
  // The sink checkpoints after session_stopping and before runtime teardown.
  automation_state_due.reset();
}

void InputSession::checkpoint_automation_now(const TimePoint now) {
  checkpoint_automation_state(now, true);
}

void X11InputService::attach(layerc::platform::X11Display &display) noexcept {
  display_ = &display;
}

void X11SessionRegistry::refresh(
    const std::span<const layerc::platform::StoredProfile> profiles,
    const layerc::platform::X11WindowScan &scan) {
  auto previous = std::move(sessions_);
  std::vector<InputSession> next;
  automation_diagnostics_.clear();
  visual_diagnostics_.clear();
  for (const auto &source : profiles) {
    const auto &profile = source.profile;
    if (!profile.enabled) continue;
    auto visual = load_visual(source);
    if (!visual.diagnostic.empty())
      visual_diagnostics_.push_back(std::move(visual.diagnostic));
    if (profile.bindings.empty() && !visual.profile.has_value() &&
        !visual.invalid) continue;
    const auto previous_session =
        std::ranges::find(previous, source.path, &InputSession::source_path);
    const auto previous_window = previous_session == previous.end()
                                     ? 0 : previous_session->window;
    const auto *window =
        layerc::platform::match_window(scan, profile.game, previous_window);
    if (window == nullptr) continue;

    auto loaded = load_automation(source);
    if (!loaded.diagnostic.empty())
      automation_diagnostics_.push_back(std::move(loaded.diagnostic));
    // Foreground eligibility is runtime state, not profile identity: a
    // background game keeps its automation profile and cooldown engine so
    // focus-away/back never reconstructs the session. Dispatch still gates
    // event feeding on session.active below.
    const bool active = profile.game.global || window->active;

    const auto retained = std::ranges::find_if(
        previous, [&](const InputSession &session) {
          const bool same_target =
              profile.game.global || session.window == window->window;
          return same_target && session.source_path == source.path &&
                 session.profile == profile &&
                 same_visual(session, visual.profile) &&
                 same_automation(session, loaded.profile);
        });
    if (retained == previous.end()) {
      const auto stale = std::ranges::find(previous, source.path,
                                           &InputSession::source_path);
      if (stale != previous.end()) {
        stale->stop_automation(layerc::core::MonotonicClock::now());
        previous.erase(stale);
        if (loaded.profile.has_value()) {
          const auto state = layerc::platform::load_automation_state(
              source.path, *loaded.profile);
          loaded.state = std::move(state.values);
        }
      }
      next.emplace_back(source, window->window, profile.game.global,
                        std::move(visual.profile), std::move(loaded.profile),
                        std::move(loaded.state), visual.invalid);
    } else {
      if (retained->automation_runtime &&
          !retained->automation_runtime->persistent_dirty())
        retained->automation_runtime->apply_persistent_values(loaded.state);
      next.push_back(std::move(*retained));
      previous.erase(retained);
      next.back().window = window->window;
      next.back().set_invalid_visual_sidecar(visual.invalid);
    }
    if (!active) next.back().matcher.clear();
    next.back().active = active;
  }
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : previous) session.stop_automation(now);
  sessions_ = std::move(next);
}

void X11SessionRegistry::stop_sessions(const TimePoint now) {
  for (auto &session : sessions_) session.stop_automation(now);
  sessions_.clear();
}

std::vector<layerc::runtime::SessionCapabilityRequest>
X11SessionRegistry::session_requests() const {
  std::vector<layerc::runtime::SessionCapabilityRequest> requests;
  requests.reserve(sessions_.size());
  for (const auto &session : sessions_) {
    layerc::runtime::SessionCapabilityRequest request;
    request.window = layerc::runtime::WindowId{session.window};
    request.active = session.active;
    request.global = session.global;
    request.visual = !session.global && session.visual_profile.has_value();
    request.isolated_visual =
        request.visual &&
        session.profile.visual_runtime == layerc::core::VisualRuntime::isolated;
    request.automation = session.automation_runtime != nullptr;
    request.profile_id = session.profile.id;
    requests.push_back(request);
  }
  return requests;
}

X11InputDispatch X11InputService::dispatch() {
  X11InputDispatch result;
  if (!source_.has_value()) return result;
  auto poll = source_->poll();
  result.events = poll.events.size();
  result.windows_changed = poll.windows_changed;
  result.devices_changed = poll.devices_changed;
  if (poll.events.empty() || registry_.sessions().empty()) return result;
  const auto now = layerc::core::MonotonicClock::now();
  for (const auto &event : poll.events)
    for (auto &session : registry_.sessions()) {
      if (!session.global && !session.active) continue;
      auto triggered = session.feed(event, now);
      result.triggered.insert(result.triggered.end(),
                              std::make_move_iterator(triggered.begin()),
                              std::make_move_iterator(triggered.end()));
    }
  return result;
}

std::vector<std::string> X11InputService::unavailable() const {
  if (!source_.has_value()) return {"input: no profile session demands input"};
  return source_->unavailable();
}

std::size_t X11SessionRegistry::automation_count() const noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      sessions_, [](const auto &session) { return session.automation_runtime != nullptr; }));
}

void X11SessionRegistry::poll_automation(const TimePoint now) {
  for (auto &session : sessions_) session.poll_automation(now);
}

std::optional<TimePoint>
X11SessionRegistry::next_automation_deadline(const TimePoint now) {
  std::optional<TimePoint> next;
  for (auto &session : sessions_)
    next = layerc::core::earliest_deadline(
        next, session.next_automation_deadline(now));
  return next;
}

layerc::runtime::ServiceReconcileResult
X11InputService::reconcile(
    const layerc::runtime::CapabilityDemand &demand) {
  if (!demand.required(layerc::runtime::Capability::input))
    return {false, "input: service invoked without demand"};
  if (source_.has_value()) return {};
  if (display_ == nullptr || !display_->valid())
    return {false, "input: no X display for the input service"};
  std::string error;
  auto source = layerc::platform::X11InputSource::create(*display_, error);
  if (!source.has_value()) return {false, error};
  source_ = std::move(*source);
  return {};
}

void X11InputService::quiesce() noexcept {
  // Sessions outlive the XI2 subscription: zero input demand releases only
  // the device source so running cooldowns and automation runtimes survive.
  // Automation state is owned by X11AutomationService; session objects
  // are retired only by refresh() when their profile/window no longer
  // resolves.
  source_.reset();
  for (auto &session : registry_.sessions()) session.matcher.clear();
}

layerc::runtime::RuntimeResourceStats
X11InputService::resources() const noexcept {
  layerc::runtime::RuntimeResourceStats stats;
  stats.handles = source_.has_value() ? 1U : 0U;
  stats.objects = static_cast<std::uint32_t>(
      std::min<std::size_t>(registry_.session_count(), 0xFFFFFFFFU));
  return stats;
}

layerc::runtime::ServiceReconcileResult
X11AutomationService::reconcile(
    const layerc::runtime::CapabilityDemand &demand) {
  if (!demand.required(layerc::runtime::Capability::automation))
    return {false, "automation: service invoked without demand"};
  return {};
}

void X11AutomationService::quiesce() noexcept {
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : registry_.sessions()) {
    try {
      session.checkpoint_automation_now(now);
    } catch (...) {
    }
  }
}

layerc::runtime::RuntimeResourceStats
X11AutomationService::resources() const noexcept {
  layerc::runtime::RuntimeResourceStats stats;
  const auto now = layerc::core::MonotonicClock::now();
  try {
    for (auto &session : registry_.sessions()) {
      if (!session.automation_runtime) continue;
      ++stats.objects;
      const auto &snapshot = session.automation_runtime->snapshot();
      stats.cache_bytes +=
          snapshot.variables.size() * sizeof(layerc::core::AutomationValue);
      stats.cache_bytes +=
          snapshot.timers.size() * sizeof(layerc::core::AutomationTimer);
      stats.cache_bytes += snapshot.overlay_overrides.size() *
                           sizeof(layerc::core::OverlayPropertyOverride);
      if (session.next_automation_deadline(now).has_value()) stats.timers = 1U;
    }
  } catch (...) {
  }
  return stats;
}

} // namespace layerc::daemon
