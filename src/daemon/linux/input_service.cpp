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

// One profile's module sidecars as value copies. Definitions without an
// enabled entry are dropped so they demand no capability.
[[nodiscard]] ConfiguredProfile
load_configured(const layerc::platform::StoredProfile &source) {
  ConfiguredProfile bundle{
      .source = source,
      .visual = layerc::platform::ModuleSidecar<VisualProfile>::from(
          layerc::platform::load_visual_sidecar(source.path, source.profile)),
      .automation = layerc::platform::ModuleSidecar<AutomationProfile>::from(
          layerc::platform::load_automation_sidecar(source.path,
                                                    source.profile))};
  if (bundle.visual.profile &&
      !layerc::core::has_enabled_visual_assignments(*bundle.visual.profile))
    bundle.visual.profile.reset();
  if (bundle.automation.profile &&
      !layerc::core::has_enabled_automations(*bundle.automation.profile))
    bundle.automation.profile.reset();
  return bundle;
}

[[nodiscard]] bool
same_visual(const InputSession &session,
            const std::optional<VisualProfile> &profile) noexcept {
  return session.visual_profile == profile;
}

[[nodiscard]] bool same_automation(
    const InputSession &session,
    const std::optional<AutomationProfile> &profile) noexcept {
  if (!profile.has_value()) return session.automation.runtime() == nullptr;
  return session.automation.runtime() != nullptr &&
         session.automation.runtime()->profile() == *profile;
}

[[nodiscard]] layerc::platform::X11InputCapabilities
needed_devices(std::span<const InputSession> sessions);

} // namespace

// The adapter supplies only durable storage. All event, validation,
// cooldown, origin, and fault policy lives in core::AutomationSession; the
// persistent values belong to the registry's AutomationPersistence.
namespace {
class InputSessionSink final : public layerc::core::AutomationSessionSink {
public:
  explicit InputSessionSink(InputSession &session) : session_(session) {}
  void on_persistent_change(const AutomationPersistentValues &values,
                            const TimePoint now) override {
    if (session_.persistence != nullptr)
      session_.persistence->publish(session_.source_path, values, now);
  }
  void on_stopping(const TimePoint now) override {
    if (session_.persistence == nullptr || !session_.automation.runtime())
      return;
    session_.persistence->publish(
        session_.source_path, session_.automation.runtime()->persistent_values(),
        now);
    // The final write happens now; a failure stays pending in the owner,
    // visible in its diagnostics, and is retried after the session is gone.
    // The runtime is retiring, so there is nothing to acknowledge.
    session_.persistence->checkpoint(now, true, &session_.source_path);
  }

private:
  InputSession &session_;
};
} // namespace

InputSession::InputSession(const ConfiguredProfile &bundle,
                           const std::uint64_t window,
                           layerc::platform::AutomationPersistence &owner)
    : automation(bundle.source.profile),
      source_path(bundle.source.path), window(window),
      global(bundle.source.profile.game.global), matcher(automation.profile().bindings),
      visual_profile(bundle.visual_profile()),
      invalid_automation_sidecar(bundle.invalid_automation()),
      persistence(&owner) {
  automation.set_invalid_visual_sidecar(bundle.invalid_visual());
  automation.configure_visual(visual_profile);
  if (const auto &automation = bundle.automation_profile()) {
    const auto now = layerc::core::MonotonicClock::now();
    const auto &state = owner.attach(source_path, *automation, now);
    InputSessionSink sink(*this);
    (void)this->automation.start(*automation, state.values, now, &sink);
  }
}

std::vector<std::string>
InputSession::feed(const layerc::core::InputEvent &input, const TimePoint now) {
  InputSessionSink sink(*this);
  auto result = automation.feed_input(matcher, input, now, &sink);
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
  return automation.feed_measurements(
      measurements, now, &sink).triggered;
}

void InputSession::poll_automation(const TimePoint now) {
  InputSessionSink sink(*this);
  (void)automation.poll(now, &sink);
}

std::optional<TimePoint>
InputSession::next_automation_deadline(const TimePoint now) {
  return automation.next_deadline(now);
}

void InputSession::stop_automation(const TimePoint now) {
  InputSessionSink sink(*this);
  (void)automation.stop(now, &sink);
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
    configured_.push_back(load_configured(source));
    const auto &loaded = configured_.back();
    if (loaded.visual_profile() &&
        loaded.source.profile.visual_runtime ==
            layerc::core::VisualRuntime::inline_process)
      visual_configurations_.push_back(
          {loaded.source.path, loaded.source.profile.id,
           *loaded.visual_profile()});
  }
  configuration_changed_ = true;
}

void X11SessionRegistry::refresh(const layerc::platform::X11WindowScan &scan) {
  auto previous = std::move(sessions_);
  std::vector<InputSession> next;
  visual_diagnostics_.clear();
  matched_automation_diagnostics_.clear();
  const auto now = layerc::core::MonotonicClock::now();
  for (const auto &bundle : configured_) {
    const auto &source = bundle.source;
    const auto &profile = source.profile;
    // Admission follows the complete loaded bundle, independent of whether
    // XI2 has any direct binding to feed.
    if (profile.bindings.empty() && profile.panels.empty() &&
        !bundle.visual_profile() && !bundle.automation_profile() &&
        !bundle.invalid_visual() && !bundle.invalid_automation())
      continue;
    const auto previous_session =
        std::ranges::find(previous, source.path, &InputSession::source_path);
    const auto previous_window = previous_session == previous.end()
                                     ? 0 : previous_session->window;
    const auto *window =
        layerc::platform::match_window(scan, profile.game, previous_window);
    if (window == nullptr) continue;
    if (bundle.invalid_visual())
      visual_diagnostics_.push_back(source.path.string() + ": " +
                                    bundle.visual.diagnostic);
    if (bundle.invalid_automation())
      matched_automation_diagnostics_.push_back(source.path.string() + ": " +
                                                bundle.automation.diagnostic);

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
                 session.automation.profile() == profile &&
                 same_visual(session, bundle.visual_profile()) &&
                 same_automation(session, bundle.automation_profile());
        });
    if (retained == previous.end()) {
      const auto stale = std::ranges::find(previous, source.path,
                                           &InputSession::source_path);
      if (stale != previous.end()) {
        stale->stop_automation(now);
        previous.erase(stale);
      }
      // A replacement continues from the owner's values, including unsaved
      // ones whose write is still being retried.
      next.emplace_back(bundle, window->window, persistence_);
    } else {
      if (configuration_changed_ && retained->automation.runtime()) {
        // An explicit reload re-reads the durable state unless values are
        // still unsaved.
        auto &runtime = *retained->automation.runtime();
        const auto &state =
            persistence_.attach(source.path, runtime.profile(), now, true);
        if (!runtime.persistent_dirty())
          runtime.apply_persistent_values(state.values);
        // Values the store lacked (an unreadable store) are rewritten from
        // the runtime instead of from the empty reload.
        persistence_.publish(source.path, runtime.persistent_values(), now);
      }
      next.push_back(std::move(*retained));
      previous.erase(retained);
      next.back().window = window->window;
      next.back().automation.set_invalid_visual_sidecar(bundle.invalid_visual());
      next.back().invalid_automation_sidecar = bundle.invalid_automation();
    }
    if (!active) next.back().matcher.clear();
    next.back().active = active;
  }
  for (auto &session : previous) session.stop_automation(now);
  sessions_ = std::move(next);
  std::vector<std::filesystem::path> live;
  for (const auto &session : sessions_)
    if (session.automation.runtime()) live.push_back(session.source_path);
  persistence_.retain(live);
  configuration_changed_ = false;
}

void X11SessionRegistry::acknowledge_saved(
    const std::span<const std::filesystem::path> saved) {
  for (const auto &path : saved) {
    const auto *entry = persistence_.find(path);
    if (entry == nullptr) continue;
    for (auto &session : sessions_) {
      if (session.source_path != path || !session.automation.runtime()) continue;
      if (session.automation.runtime()->persistent_values() == entry->values)
        session.automation.runtime()->mark_persistent_saved();
    }
  }
}

std::size_t X11SessionRegistry::pending_checkpoint_count() const {
  return static_cast<std::size_t>(std::ranges::count_if(
      persistence_.pending_paths(), [&](const std::filesystem::path &path) {
        return std::ranges::none_of(sessions_, [&](const InputSession &session) {
          return session.source_path == path && session.automation.runtime();
        });
      }));
}

std::vector<std::string> X11SessionRegistry::automation_diagnostics() const {
  auto diagnostics = matched_automation_diagnostics_;
  for (auto &line : persistence_.diagnostics())
    diagnostics.push_back(std::move(line));
  for (const auto &session : sessions_)
    for (auto &line :
         session.automation.health().lines(session.source_path.string()))
      diagnostics.push_back(std::move(line));
  return diagnostics;
}

void X11SessionRegistry::stop_sessions(const TimePoint now) {
  for (auto &session : sessions_) session.stop_automation(now);
  sessions_.clear();
  // Shutdown flush: no session is left to acknowledge; a failed write stays
  // pending in the owner with its fault visible in diagnostics.
  persistence_.checkpoint(now, true);
}

void X11SessionRegistry::checkpoint_automation(const TimePoint now) {
  acknowledge_saved(persistence_.checkpoint(now, true));
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
    const auto devices =
        needed_devices(std::span<const InputSession>{&session, 1});
    request.input = devices.keyboard || devices.mouse || devices.gamepad;
    request.overlay = !session.automation.profile().panels.empty();
    request.visual = !session.global && session.visual_profile.has_value();
    request.isolated_visual =
        request.visual &&
        session.automation.profile().visual_runtime == layerc::core::VisualRuntime::isolated;
    request.automation = session.automation.runtime() != nullptr ||
                         session.invalid_automation_sidecar;
    request.profile_id = session.automation.profile().id;
    requests.push_back(request);
  }
  // Unsaved values without a session keep the automation service (and its
  // retry deadline) alive until their write succeeds.
  for (std::size_t index = 0; index < pending_checkpoint_count(); ++index) {
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
    for (const auto &binding : session.automation.profile().bindings)
      for (const auto &token : binding.chord.tokens) mark(token.device);
    if (session.automation.runtime() == nullptr) continue;
    for (const auto &automation :
         session.automation.runtime()->profile().automations) {
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
      sessions_, [](const auto &session) { return session.automation.runtime() != nullptr; }));
}

bool X11SessionRegistry::poll_automation(const TimePoint now) {
  for (auto &session : sessions_) session.poll_automation(now);
  // A completed write of a session-less entry drops its automation demand.
  const auto orphaned = pending_checkpoint_count();
  acknowledge_saved(persistence_.checkpoint(now));
  return pending_checkpoint_count() != orphaned;
}

std::optional<TimePoint>
X11SessionRegistry::next_automation_deadline(const TimePoint now) {
  std::optional<TimePoint> next = persistence_.next_deadline();
  for (auto &session : sessions_)
    next = layerc::core::earliest_deadline(
        next, session.next_automation_deadline(now));
  return next;
}

layerc::runtime::ServiceReconcileResult
X11InputService::reconcile(
    const layerc::runtime::CapabilityDemand &demand) {
  if (!demand.required(layerc::runtime::Capability::input))
    return layerc::runtime::ServiceReconcileResult::failed(
        "input: service invoked without demand");
  if (source_.has_value()) return {};
  if (display_ == nullptr || !display_->valid())
    return layerc::runtime::ServiceReconcileResult::failed(
        "input: no X display for the input service");
  std::string error;
  auto source = layerc::platform::X11InputSource::create(*display_, error);
  if (!source.has_value())
    return layerc::runtime::ServiceReconcileResult::failed(std::move(error));
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
    return layerc::runtime::ServiceReconcileResult::failed(
        "automation: service invoked without demand");
  return {};
}

void X11AutomationService::quiesce() noexcept {
  try {
    registry_.checkpoint_automation(layerc::core::MonotonicClock::now());
  } catch (...) {
  }
}

layerc::runtime::RuntimeResourceStats
X11AutomationService::resources() const noexcept {
  layerc::runtime::RuntimeResourceStats stats;
  const auto now = layerc::core::MonotonicClock::now();
  try {
    const auto pending = registry_.pending_checkpoint_count();
    stats.objects =
        static_cast<std::uint32_t>(std::min<std::size_t>(pending, 0xFFFFFFFFU));
    if (pending != 0) stats.timers = 1U;
    for (auto &session : registry_.sessions()) {
      if (!session.automation.runtime()) continue;
      ++stats.objects;
      const auto &snapshot = session.automation.runtime()->snapshot();
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
