#include "input_service.hpp"

#include "layerc/core/automation_session.hpp"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <string>
#include <utility>

namespace layerc::daemon {

void InputService::use_sources(const std::span<PassiveInputSource *const> ordered) {
  std::vector<PassiveInputSource *> next;
  for (auto *source : ordered)
    if (source && std::ranges::find(next, source) == next.end()) next.push_back(source);
  if (sources_ == next) return;
  bool released = false;
  for (auto *source : sources_)
    if (std::ranges::find(next, source) == next.end()) {
      released = released || source->input_open();
      source->close_input();
    }
  sources_ = std::move(next);
  if (released)
    for (auto &session : registry_.sessions()) session.matcher.clear();
}

void InputService::release() noexcept {
  for (auto *source : sources_) source->close_input();
  for (auto &session : registry_.sessions()) session.matcher.clear();
}

PassiveInputSource *InputService::attached() const noexcept {
  for (auto *source : sources_)
    if (source->input_open()) return source;
  return nullptr;
}

DisplayDispatch InputService::poll(const std::span<const int> ready) {
  DisplayDispatch result;
  auto *source = attached();
  if (!source) return result;
  auto polled = source->poll_input(ready);
  result.events = polled.events.size();
  result.devices_changed = polled.devices_changed;
  feed(polled.events, result);
  return result;
}

std::vector<std::string> InputService::unavailable() const {
  const auto needed = needed_devices(registry_.sessions());
  if (auto *source = attached()) return source->input_unavailable(needed);
  // A matched timer/visual/overlay session does not require a passive source
  // unless a direct binding or automation input rule can consume an event.
  if (!needed.keyboard && !needed.mouse && !needed.gamepad) return {};
  // A permission or keymap fault stays visible even when no source attaches.
  for (auto *source : sources_)
    if (!source->input_fault().empty()) return source->input_unavailable(needed);
  return {sources_.empty() ? std::string{"input: XI2 source is not attached"}
                          : "input: " + std::string{sources_.back()->input_name()} + " source is not attached"};
}

// All passive sources feed sessions under the same rules.
void InputService::feed(const std::span<const layerc::core::InputEvent> events,
                           DisplayDispatch &result) {
  if (events.empty() || registry_.sessions().empty()) return;
  const auto now = layerc::core::MonotonicClock::now();
  for (const auto &event : events)
    for (auto &session : registry_.sessions()) {
      if (!session.global && !session.active) continue;
      auto triggered = session.feed(event, now);
      result.triggered.insert(result.triggered.end(),
                              std::make_move_iterator(triggered.begin()),
                              std::make_move_iterator(triggered.end()));
    }
}

layerc::runtime::RuntimeResourceStats
InputService::resources() const noexcept {
  layerc::runtime::RuntimeResourceStats stats;
  for (auto *source : sources_) stats += source->input_resources();
  return stats;
}

layerc::runtime::ServiceReconcileResult InputService::reconcile(
    const layerc::runtime::CapabilityDemand &demand) {
  if (!demand.required(layerc::runtime::Capability::input))
    return layerc::runtime::ServiceReconcileResult::failed("input: service invoked without demand");
  if (attached()) return {};
  if (sources_.empty())
    return layerc::runtime::ServiceReconcileResult::failed("input: no X display for the input service");
  std::string error;
  for (auto *source : sources_) {
    error.clear();
    if (source->open_input(error)) return {};
  }
  return layerc::runtime::ServiceReconcileResult::failed(std::move(error));
}

// Sessions outlive the passive subscription: zero input demand releases only
// the sources so running cooldowns and automation runtimes survive. Automation
// state is owned by AutomationService; session objects are retired only by
// refresh() when their profile/window no longer resolves.
void InputService::quiesce() noexcept { release(); }

layerc::runtime::ServiceReconcileResult
AutomationService::reconcile(
    const layerc::runtime::CapabilityDemand &demand) {
  if (!demand.required(layerc::runtime::Capability::automation))
    return layerc::runtime::ServiceReconcileResult::failed(
        "automation: service invoked without demand");
  return {};
}

void AutomationService::quiesce() noexcept {
  try {
    registry_.checkpoint_automation(layerc::core::MonotonicClock::now());
  } catch (...) {
  }
}

layerc::runtime::RuntimeResourceStats
AutomationService::resources() const noexcept {
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
