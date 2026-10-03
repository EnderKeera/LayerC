// X11 input capability (V2.6.1): profile sessions driven by passive XI2 raw
// input, registered with the daemon kernel as Capability::input.
#include "input_service.hpp"

#include "layerc/core/automation_session.hpp"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <string>
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
  std::string sidecar_diagnostic;
  std::optional<std::string> state_diagnostic;
  bool invalid{};
};

[[nodiscard]] LoadedVisual
load_visual(const layerc::platform::StoredProfile &source) {
  LoadedVisual result;
  auto sidecar =
      layerc::platform::load_visual_sidecar(source.path, source.profile);
  if (!sidecar.usable()) {
    if (sidecar.state() == layerc::platform::ModuleSidecarState::invalid) {
      result.invalid = true;
      result.diagnostic = source.path.string() + ": " + sidecar.error()->message;
    }
    return result;
  }
  if (layerc::core::has_enabled_visual_assignments(*sidecar.profile()))
    result.profile = sidecar.optional_profile();
  return result;
}

[[nodiscard]] LoadedAutomation
load_automation(const layerc::platform::StoredProfile &source) {
  LoadedAutomation result;
  auto sidecar = layerc::platform::load_automation_sidecar(source.path,
                                                           source.profile);
  if (!sidecar.usable()) {
    if (sidecar.state() == layerc::platform::ModuleSidecarState::invalid) {
      result.invalid = true;
      result.sidecar_diagnostic = source.path.string() + ": " +
                                  sidecar.error()->message;
    }
    return result;
  }

  if (layerc::core::has_enabled_automations(*sidecar.profile())) {
    auto state = layerc::platform::load_automation_state(source.path,
                                                         *sidecar.profile());
    result.state = std::move(state.values);
    if (state.error.has_value())
      result.state_diagnostic = source.path.string() + ": " +
                                state.error->message;
    result.profile = sidecar.optional_profile();
  }
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

[[nodiscard]] layerc::platform::X11InputCapabilities
needed_devices(std::span<const InputSession> sessions);

} // namespace

// The adapter supplies only durable storage and fault reporting. All event,
// validation, cooldown and origin policy lives in core::AutomationSession.
namespace {
class InputSessionSink final : public layerc::core::AutomationSessionSink {
public:
  explicit InputSessionSink(
      InputSession &session,
      std::optional<RetiredAutomationState> *retired = nullptr)
      : session_(session), retired_(retired) {}
  void on_result(const layerc::core::AutomationEvaluationResult &result) override {
    auto &health = session_.automation_health;
    const auto error = result.error.has_value()
                           ? result.error->path + ": " + result.error->message
                           : std::string{};
    if (result.faulted_automations.empty()) {
      if (result.error)
        health.runtime_error = error;
      else
        health.runtime_error.reset();
    } else {
      health.runtime_error.reset();
      for (const auto &id : result.faulted_automations)
        health.runtime_faults.insert_or_assign(id, "faulted during event");
    }
  }
  void on_persistent_change(const AutomationPersistentValues &,
                            const TimePoint now) override {
    layerc::core::automation_checkpoint_arm(session_.automation_state_due, now);
  }
  void on_stopping(const TimePoint now) override {
    session_.checkpoint_automation_now(now);
    if (retired_ == nullptr || !session_.automation_runtime ||
        !session_.automation_state_due.has_value())
      return;
    *retired_ = RetiredAutomationState{
        session_.automation_runtime->profile(),
        session_.automation_runtime->snapshot().persistent_values,
        session_.automation_state_due,
        session_.automation_health.checkpoint_error};
  }
private:
  InputSession &session_;
  std::optional<RetiredAutomationState> *retired_{};
};
} // namespace

InputSession::InputSession(
    layerc::platform::StoredProfile source, const std::uint64_t window,
    const bool global, std::optional<VisualProfile> loaded_visual_profile,
    std::optional<AutomationProfile> automation_profile,
    AutomationPersistentValues automation_state, const bool invalid_visual_sidecar,
    const bool loaded_invalid_automation_sidecar,
    std::optional<std::string> automation_state_load_error)
    : layerc::core::AutomationSession(std::move(source.profile)),
      source_path(std::move(source.path)), window(window), global(global),
      matcher(profile.bindings),
      visual_profile(std::move(loaded_visual_profile)),
      invalid_automation_sidecar(loaded_invalid_automation_sidecar) {
  set_invalid_visual_sidecar(invalid_visual_sidecar);
  automation_health.state_load_error =
      std::move(automation_state_load_error);
  if (automation_health.state_load_error)
    layerc::core::automation_checkpoint_arm(
        automation_state_due, layerc::core::MonotonicClock::now());
  if (visual_profile)
    visual_triggers =
        std::make_unique<layerc::core::VisualTriggerEngine>(*visual_profile);
  if (automation_profile) {
    InputSessionSink sink(*this);
    (void)start(std::move(*automation_profile), std::move(automation_state),
                layerc::core::MonotonicClock::now(), &sink);
  }
}

void InputSession::checkpoint_automation_state(const TimePoint now,
                                               const bool force) {
  if (!automation_runtime) return;
  if (automation_runtime->persistent_dirty())
    layerc::core::automation_checkpoint_arm(automation_state_due, now);
  const auto written = layerc::platform::checkpoint_automation_state(
      source_path, automation_runtime->profile(),
      automation_runtime->snapshot().persistent_values, automation_state_due,
      now, force);
  if (std::holds_alternative<layerc::platform::CheckpointNotDue>(written))
    return;
  if (const auto *error =
          std::get_if<layerc::platform::ProfileWriteError>(&written))
    automation_health.checkpoint_error =
        std::string("automation checkpoint failed (") +
        layerc::platform::profile_write_error_text(*error) +
        "): " + source_path.string();
  else {
    automation_health.checkpoint_error.reset();
    automation_health.state_load_error.reset();
    automation_runtime->mark_persistent_saved();
  }
}

std::vector<std::string>
InputSession::feed(const layerc::core::InputEvent &input, const TimePoint now) {
  InputSessionSink sink(*this);
  auto result = feed_input(matcher, input, now, &sink);
  if (result.matched)
    last_input_match =
        std::string(layerc::core::input_device_label(input.token.device)) +
        ":" + std::to_string(input.token.code) +
        (input.pressed ? ":press" : ":release");
  return std::move(result.triggered);
}

std::vector<std::string> InputSession::feed_measurements(
    const std::span<const layerc::core::ObservationMeasurement> measurements,
    const TimePoint now) {
  InputSessionSink sink(*this);
  return layerc::core::AutomationSession::feed_measurements(
      measurements, now, &sink).triggered;
}

void InputSession::poll_automation(const TimePoint now) {
  InputSessionSink sink(*this);
  (void)poll(now, &sink);
  checkpoint_automation_state(now, false);
}

std::optional<TimePoint>
InputSession::next_automation_deadline(const TimePoint now) {
  return next_deadline(now, automation_state_due);
}

std::optional<RetiredAutomationState>
InputSession::stop_automation(const TimePoint now) {
  std::optional<RetiredAutomationState> retired;
  InputSessionSink sink(*this, &retired);
  (void)stop(now, &sink);
  // The sink checkpoints after session_stopping and before runtime teardown.
  automation_state_due.reset();
  return retired;
}

void InputSession::start_deferred_automation(
    const AutomationProfile &automation_profile, AutomationPersistentValues values,
    const TimePoint now, std::optional<std::string> state_load_error) {
  deferred_automation = false;
  automation_health.state_load_error = std::move(state_load_error);
  if (automation_health.state_load_error)
    layerc::core::automation_checkpoint_arm(automation_state_due, now);
  InputSessionSink sink(*this);
  (void)start(automation_profile, std::move(values), now, &sink);
}

void InputSession::checkpoint_automation_now(const TimePoint now) {
  checkpoint_automation_state(now, true);
}

void X11InputService::attach(layerc::platform::X11Display &display) noexcept {
  display_ = &display;
}

void X11SessionRegistry::configure(
    const std::span<const layerc::platform::StoredProfile> profiles) {
  visual_configurations_.clear();
  configured_.clear();
  configured_.reserve(profiles.size());
  visual_configurations_.reserve(profiles.size());
  for (const auto &source : profiles) {
    if (!source.profile.enabled) continue;
    auto visual = load_visual(source);
    auto automation = load_automation(source);
    configured_.push_back({source, std::move(visual.profile),
                           std::move(automation.profile),
                           std::move(automation.state),
                           std::move(visual.diagnostic),
                           std::move(automation.sidecar_diagnostic),
                           std::move(automation.state_diagnostic),
                           visual.invalid, automation.invalid});
    const auto &loaded = configured_.back();
    if (loaded.visual_profile &&
        loaded.source.profile.visual_runtime ==
            layerc::core::VisualRuntime::inline_process)
      visual_configurations_.push_back(
          {loaded.source.path, loaded.source.profile.id,
           *loaded.visual_profile});
  }
  configuration_changed_ = true;
}

std::optional<RetiredAutomationState>
X11SessionRegistry::retire_session(InputSession &session, const TimePoint now) {
  auto retired = session.stop_automation(now);
  if (retired.has_value())
    pending_checkpoints_.try_emplace(session.source_path, *retired);
  return retired;
}

bool X11SessionRegistry::retry_pending_checkpoints(const TimePoint now,
                                                    const bool force) {
  bool changed{};
  for (auto entry = pending_checkpoints_.begin();
       entry != pending_checkpoints_.end();) {
    auto &retired = entry->second;
    const auto written = layerc::platform::checkpoint_automation_state(
        entry->first, retired.profile, retired.values, retired.due, now, force);
    if (std::holds_alternative<layerc::platform::CheckpointNotDue>(written)) {
      ++entry;
      continue;
    }
    if (const auto *error =
            std::get_if<layerc::platform::ProfileWriteError>(&written)) {
      retired.checkpoint_error =
          std::string("automation checkpoint failed (") +
          layerc::platform::profile_write_error_text(*error) +
          "): " + entry->first.string();
      ++entry;
      continue;
    }
    const auto path = entry->first;
    entry = pending_checkpoints_.erase(entry);
    changed = true;
    const auto configured = std::ranges::find_if(
        configured_, [&](const ConfiguredProfile &bundle) {
          return bundle.source.path == path;
        });
    if (configured == configured_.end() ||
        !configured->automation_profile.has_value())
      continue;
    for (auto &session : sessions_) {
      if (session.source_path != path || !session.deferred_automation) continue;
      auto state = layerc::platform::load_automation_state(
          path, *configured->automation_profile);
      std::optional<std::string> load_error;
      if (state.error)
        load_error = path.string() + ": " + state.error->message;
      session.start_deferred_automation(*configured->automation_profile,
                                        std::move(state.values), now,
                                        std::move(load_error));
    }
  }
  return changed;
}

void X11SessionRegistry::refresh(const layerc::platform::X11WindowScan &scan) {
  auto previous = std::move(sessions_);
  std::vector<InputSession> next;
  visual_diagnostics_.clear();
  matched_automation_diagnostics_.clear();
  for (const auto &bundle : configured_) {
    const auto &source = bundle.source;
    const auto &profile = source.profile;
    // Admission follows the complete loaded bundle, independent of whether
    // XI2 has any direct binding to feed.
    if (profile.bindings.empty() && profile.panels.empty() &&
        !bundle.visual_profile && !bundle.automation_profile &&
        !bundle.invalid_visual_sidecar &&
        !bundle.invalid_automation_sidecar)
      continue;
    const auto previous_session =
        std::ranges::find(previous, source.path, &InputSession::source_path);
    const auto previous_window = previous_session == previous.end()
                                     ? 0 : previous_session->window;
    const auto *window =
        layerc::platform::match_window(scan, profile.game, previous_window);
    if (window == nullptr) continue;
    if (!bundle.visual_diagnostic.empty())
      visual_diagnostics_.push_back(bundle.visual_diagnostic);
    if (!bundle.automation_sidecar_diagnostic.empty())
      matched_automation_diagnostics_.push_back(
          bundle.automation_sidecar_diagnostic);

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
                 same_visual(session, bundle.visual_profile) &&
                 (session.deferred_automation
                      ? bundle.automation_profile.has_value() &&
                            pending_checkpoints_.contains(source.path)
                      : same_automation(session, bundle.automation_profile));
        });
    if (retained == previous.end()) {
      const auto stale = std::ranges::find(previous, source.path,
                                           &InputSession::source_path);
      if (stale != previous.end()) {
        (void)retire_session(*stale, layerc::core::MonotonicClock::now());
        previous.erase(stale);
      }
      const bool deferred = bundle.automation_profile.has_value() &&
                            pending_checkpoints_.contains(source.path);
      AutomationPersistentValues state;
      std::optional<std::string> state_error;
      if (bundle.automation_profile.has_value() && !deferred) {
        // Configuration snapshots apply only to explicit retained-runtime
        // reloads. Every fresh admission follows the latest durable checkpoint,
        // including a predecessor that retired during an unmatched interval.
        auto saved = layerc::platform::load_automation_state(
            source.path, *bundle.automation_profile);
        state = std::move(saved.values);
        if (saved.error)
          state_error = source.path.string() + ": " + saved.error->message;
      }
      next.emplace_back(source, window->window, profile.game.global,
                        bundle.visual_profile,
                        deferred ? std::nullopt : bundle.automation_profile,
                        std::move(state), bundle.invalid_visual_sidecar,
                        bundle.invalid_automation_sidecar,
                        deferred ? std::nullopt : std::move(state_error));
      next.back().deferred_automation = deferred;
    } else {
      if (configuration_changed_ && retained->automation_runtime &&
          !retained->automation_runtime->persistent_dirty())
        retained->automation_runtime->apply_persistent_values(
            bundle.automation_state);
      next.push_back(std::move(*retained));
      previous.erase(retained);
      next.back().window = window->window;
      next.back().set_invalid_visual_sidecar(bundle.invalid_visual_sidecar);
      next.back().invalid_automation_sidecar =
          bundle.invalid_automation_sidecar;
      if (configuration_changed_ && next.back().automation_runtime) {
        next.back().automation_health.state_load_error =
            bundle.automation_state_diagnostic;
        if (bundle.automation_state_diagnostic)
          layerc::core::automation_checkpoint_arm(
              next.back().automation_state_due,
              layerc::core::MonotonicClock::now());
      }
    }
    if (!active) next.back().matcher.clear();
    next.back().active = active;
  }
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : previous) (void)retire_session(session, now);
  sessions_ = std::move(next);
  configuration_changed_ = false;
}

std::vector<std::string> X11SessionRegistry::automation_diagnostics() const {
  auto diagnostics = matched_automation_diagnostics_;
  for (const auto &session : sessions_) {
    const auto &health = session.automation_health;
    if (health.checkpoint_error)
      diagnostics.push_back(*health.checkpoint_error);
    if (health.state_load_error)
      diagnostics.push_back(*health.state_load_error);
    if (health.runtime_error)
      diagnostics.push_back(session.source_path.string() + ": " +
                            *health.runtime_error);
    for (const auto &[id, error] : health.runtime_faults)
      diagnostics.push_back(session.source_path.string() +
                            ": automation fault: " + id + ": " + error);
  }
  for (const auto &[path, retired] : pending_checkpoints_) {
    (void)path;
    if (retired.checkpoint_error)
      diagnostics.push_back(*retired.checkpoint_error);
  }
  return diagnostics;
}

void X11SessionRegistry::stop_sessions(const TimePoint now) {
  for (auto &session : sessions_) (void)retire_session(session, now);
  sessions_.clear();
  (void)retry_pending_checkpoints(now, true);
}

std::vector<layerc::runtime::SessionCapabilityRequest>
X11SessionRegistry::session_requests() const {
  std::vector<layerc::runtime::SessionCapabilityRequest> requests;
  requests.reserve(sessions_.size() + pending_checkpoints_.size());
  for (const auto &session : sessions_) {
    layerc::runtime::SessionCapabilityRequest request;
    request.window = layerc::runtime::WindowId{session.window};
    request.active = session.active;
    request.global = session.global;
    const auto devices =
        needed_devices(std::span<const InputSession>{&session, 1});
    request.input = devices.keyboard || devices.mouse || devices.gamepad;
    request.overlay = !session.profile.panels.empty();
    request.visual = !session.global && session.visual_profile.has_value();
    request.isolated_visual =
        request.visual &&
        session.profile.visual_runtime == layerc::core::VisualRuntime::isolated;
    request.automation = session.automation_runtime != nullptr ||
                         session.deferred_automation ||
                         session.invalid_automation_sidecar;
    request.profile_id = session.profile.id;
    requests.push_back(request);
  }
  for (const auto &[path, retired] : pending_checkpoints_) {
    (void)retired;
    if (std::ranges::any_of(sessions_, [&](const InputSession &session) {
          return session.source_path == path && session.deferred_automation;
        }))
      continue;
    layerc::runtime::SessionCapabilityRequest request;
    request.automation = true;
    requests.push_back(request);
  }
  return requests;
}

X11Dispatch X11InputService::dispatch(layerc::platform::X11WindowTracker *tracker) {
  X11Dispatch result;
  if (!source_.has_value()) return result;
  auto poll = source_->poll(tracker);
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

namespace {

// Device classes the matched sessions can consume: binding tokens plus the
// automation input rules, mirroring the Windows daemon's registration.
layerc::platform::X11InputCapabilities
needed_devices(std::span<const InputSession> sessions) {
  using layerc::core::AutomationEventKind;
  using layerc::core::InputDevice;
  using layerc::core::source_filter_names_device;
  layerc::platform::X11InputCapabilities needed;
  const auto mark = [&](const InputDevice device) {
    needed.keyboard = needed.keyboard || device == InputDevice::keyboard;
    needed.mouse = needed.mouse || device == InputDevice::mouse;
    needed.gamepad = needed.gamepad || device == InputDevice::controller;
  };
  for (const auto &session : sessions) {
    // Only sessions dispatch() feeds can consume a device.
    if (!session.global && !session.active) continue;
    for (const auto &binding : session.profile.bindings)
      for (const auto &token : binding.chord.tokens) mark(token.device);
    if (session.automation_runtime == nullptr) continue;
    for (const auto &automation :
         session.automation_runtime->profile().automations) {
      if (!automation.enabled) continue;
      for (const auto &rule : automation.rules) {
        const bool edge = rule.event == AutomationEventKind::input_pressed ||
                          rule.event == AutomationEventKind::input_released;
        const bool value =
            rule.event == AutomationEventKind::input_value_changed;
        if (!edge && !value) continue;
        for (const auto device : {InputDevice::keyboard, InputDevice::mouse,
                                  InputDevice::controller})
          if (rule.source_filter.empty() ||
              source_filter_names_device(rule.source_filter, device))
            mark(device);
      }
    }
  }
  return needed;
}

} // namespace

std::vector<std::string> X11InputService::unavailable() const {
  const auto needed = needed_devices(registry_.sessions());
  if (!source_.has_value()) {
    // A matched timer/visual/overlay session does not require an XI2 source
    // unless a direct binding or automation input rule can consume an event.
    if (!needed.keyboard && !needed.mouse && !needed.gamepad) return {};
    return {"input: XI2 source is not attached"};
  }
  return source_->unavailable(needed);
}

std::size_t X11SessionRegistry::automation_count() const noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      sessions_, [](const auto &session) { return session.automation_runtime != nullptr; }));
}

bool X11SessionRegistry::poll_automation(const TimePoint now) {
  const bool changed = retry_pending_checkpoints(now);
  for (auto &session : sessions_) session.poll_automation(now);
  return changed;
}

std::optional<TimePoint>
X11SessionRegistry::next_automation_deadline(const TimePoint now) {
  std::optional<TimePoint> next;
  for (auto &session : sessions_)
    next = layerc::core::earliest_deadline(
        next, session.next_automation_deadline(now));
  for (const auto &[path, retired] : pending_checkpoints_) {
    (void)path;
    next = layerc::core::earliest_deadline(next, retired.due);
  }
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
  stats.objects = source_.has_value() ? 1U : 0U;
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
  stats.objects = static_cast<std::uint32_t>(
      std::min<std::size_t>(registry_.pending_checkpoint_count(),
                            0xFFFFFFFFU));
  if (registry_.pending_checkpoint_count() != 0) stats.timers = 1U;
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
