#include "daemon_app.hpp"

#include "layerc/core/automation_session.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::daemon {
namespace {
using layerc::core::AutomationEvent;
using layerc::core::AutomationEventKind;
using layerc::core::AutomationSession;
using layerc::core::Milliseconds;
using layerc::core::TimePoint;

ProfileSource *find_source(std::vector<ProfileSource> &sources,
                           const std::string_view key) {
  const auto found = std::ranges::find(sources, key, &ProfileSource::key);
  return found == sources.end() ? nullptr : &*found;
}

} // namespace

void DaemonApp::reconcile_automation_runtime() {
  const auto now = layerc::core::MonotonicClock::now();
  // A new or reconfigured runtime starts with fresh session-scoped state, so
  // it seeds cooldown edges and replays session_started.
  const auto start_session = [&](Session &session) {
    poll_automation_cooldown_transitions(session, now, false);
    process_automation_event(
        session,
        AutomationSession::event(AutomationEventKind::session_started, {}, now));
  };
  for (auto &session : session_registry_.sessions) {
    if (!session->has_enabled_automations()) {
      session->automation_runtime.reset();
      continue;
    }
    auto *source = find_source(session_registry_.sources, session->source_key);
    if (source == nullptr || !source->modules.automation.profile.has_value()) {
      session->automation_runtime.reset();
      continue;
    }
    const auto &profile = *source->modules.automation.profile;
    if (!session->automation_runtime) {
      session->automation_revision = source->automation_revision;
      if (session->start(profile,
                         automation_persistence_.attach(source->path, profile, now)
                             .values,
                         now, nullptr, automation_runtime_host_.trace_subscribed))
        mark_overlays_dirty();
      after_automation_step(*session, now);
    } else if (session->automation_revision != source->automation_revision) {
      // The revision gate replaced a deep AutomationProfile compare plus a
      // cooldown-group compare that used to run on every reconcile: now they
      // run only after the source's automation-relevant content changed, and
      // they still decide whether a full reconfigure is needed.
      session->automation_revision = source->automation_revision;
      if (session->automation_runtime->profile() != profile ||
          !std::ranges::equal(
              session->automation_runtime->cooldown_groups(),
              session->profile.cooldown_groups)) {
        session->automation_runtime->reconfigure(
            profile, session->profile.cooldown_groups);
        session->automation_runtime->apply_persistent_values(
            automation_persistence_.attach(source->path, profile, now).values);
        // Reconfigure resets session-scoped state (variables, timers, edge
        // monitors), so replay session_started like a fresh runtime: rules
        // keyed on it re-run against the edited profile instead of waiting
        // for the next session.
        start_session(*session);
      }
    }
  }
  const bool active = std::ranges::any_of(session_registry_.sessions, [](const auto &session) {
    return session != nullptr && session->automation_runtime != nullptr;
  });
  automation_runtime_host_.publish_activity(active);
  schedule_automation_timer();
}

void DaemonApp::quiesce_automation_runtime() noexcept {
  if (automation_runtime_host_.timer_running && window_ != nullptr)
    KillTimer(window_, automation_timer);
  automation_runtime_host_.timer_running = false;
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : session_registry_.sessions) {
    if (!session->automation_runtime)
      continue;
    process_automation_event(
        *session,
        AutomationSession::event(AutomationEventKind::session_stopping, {}, now));
    session->automation_runtime.reset();
  }
  // Shutdown flush: force bypasses the debounce deadline. A failed write
  // stays pending in the owner with its fault visible in diagnostics.
  acknowledge_automation_saved(automation_persistence_.checkpoint(now, true));
  automation_runtime_host_.publish_activity(false);
}

void DaemonApp::retire_automation_session(Session &session,
                                          const TimePoint now) {
  if (!session.automation_runtime)
    return;
  process_automation_event(
      session,
      AutomationSession::event(AutomationEventKind::session_stopping, {}, now));
  if (const auto *source =
          find_source(session_registry_.sources, session.source_key))
    acknowledge_automation_saved(
        automation_persistence_.checkpoint(now, true, &source->path));
  session.automation_runtime.reset();
}
void DaemonApp::schedule_automation_timer() {
  if (window_ == nullptr)
    return;
  const auto now = layerc::core::MonotonicClock::now();
  std::optional<TimePoint> next = automation_persistence_.next_deadline();
  for (auto &session : session_registry_.sessions) {
    if (!session->automation_runtime)
      continue;
    next = session->next_deadline(now, next);
  }
  if (!next.has_value()) {
    if (automation_runtime_host_.timer_running)
      KillTimer(window_, automation_timer);
    automation_runtime_host_.timer_running = false;
    return;
  }
  const auto remaining =
      std::max(Milliseconds(1), std::chrono::ceil<Milliseconds>(*next - now));
  const auto delay = static_cast<UINT>(std::min<std::int64_t>(
      remaining.count(), std::numeric_limits<UINT>::max()));
  // SetTimer replaces a running timer with the same id.
  automation_runtime_host_.timer_running =
      SetTimer(window_, automation_timer, delay, nullptr) != 0;
}
void DaemonApp::poll_automation_timer() {
  if (automation_runtime_host_.timer_running)
    KillTimer(window_, automation_timer);
  automation_runtime_host_.timer_running = false;
  const auto now = layerc::core::MonotonicClock::now();
  for (auto &session : session_registry_.sessions) {
    if (!session->automation_runtime)
      continue;
    if (session->poll(now))
      mark_overlays_dirty();
    after_automation_step(*session, now);
  }
  acknowledge_automation_saved(automation_persistence_.checkpoint(now));
  schedule_automation_timer();
}

void DaemonApp::poll_automation_cooldown_transitions(Session &session,
                                                     const TimePoint now,
                                                     const bool emit) {
  if (!session.automation_runtime)
    return;
  if (session.poll_ready(now, emit))
    mark_overlays_dirty();
  if (emit) {
    after_automation_step(session, now);
    schedule_automation_timer();
  }
}
void DaemonApp::publish_automation_trace(Session &session) {
  if (!automation_runtime_host_.trace_subscribed ||
      !live_edit_host_.authenticated ||
      session.automation_runtime->trace().empty())
    return;
  layerc::core::LiveMessage message;
  message.type = layerc::core::LiveMessageType::automation_trace_event;
  message.target_window =
      reinterpret_cast<std::uintptr_t>(session.game_window);
  message.profile_key = session.source_key;
  constexpr std::size_t max_chunks_per_event = 4U;
  for (std::size_t chunk{}; chunk < max_chunks_per_event &&
                            !session.automation_runtime->trace().empty();
       ++chunk) {
    const auto &trace = session.automation_runtime->trace();
    const auto selected =
        layerc::core::live_trace_prefix_count(message, trace);
    if (selected == 0U) {
      // A single entry that cannot fit a live frame is replaced by an
      // explicit omission marker so the editor still sees progress.
      auto omitted = trace.front();
      omitted.phase = "error";
      omitted.detail = "source trace entry omitted because it exceeded the "
                       "live frame limit";
      omitted.source_event.reset();
      message.text = layerc::core::export_layerc_trace(
          std::span<const layerc::core::AutomationTraceEntry>(&omitted, 1U));
      if (!live_edit_host_.channel.send(message))
        break;
      session.automation_runtime->consume_trace(1U);
      continue;
    }
    message.text = layerc::core::export_layerc_trace(
        std::span<const layerc::core::AutomationTraceEntry>(trace.data(),
                                                            selected));
    if (!live_edit_host_.channel.send(message))
      break;
    session.automation_runtime->consume_trace(selected);
  }
}

void DaemonApp::process_automation_event(Session &session,
                                         const AutomationEvent &event) {
  if (!session.automation_runtime)
    return;
  if (session.process(event))
    mark_overlays_dirty();
  after_automation_step(session, event.timestamp);
  schedule_automation_timer();
}

// The one post-step glue: trace to the editor, then persistent values to the
// owner and same-profile siblings.
void DaemonApp::after_automation_step(Session &session, const TimePoint now) {
  publish_automation_trace(session);
  synchronize_automation_state(session, now);
}

void DaemonApp::acknowledge_automation_saved(
    const std::span<const std::filesystem::path> saved) {
  for (const auto &path : saved) {
    const auto *entry = automation_persistence_.find(path);
    if (entry == nullptr)
      continue;
    for (auto &session : session_registry_.sessions) {
      const auto *source =
          find_source(session_registry_.sources, session->source_key);
      if (!session->automation_runtime || source == nullptr ||
          source->path != path ||
          session->automation_runtime->persistent_values() != entry->values)
        continue;
      session->automation_runtime->mark_persistent_saved();
    }
  }
}
void DaemonApp::synchronize_automation_state(Session &session,
                                             const TimePoint now) {
  if (!session.automation_runtime ||
      !session.automation_runtime->persistent_dirty())
    return;
  const auto *source =
      find_source(session_registry_.sources, session.source_key);
  if (source == nullptr)
    return;
  // The owner arms the shared debounce on the first change of a burst.
  automation_persistence_.publish(
      source->path, session.automation_runtime->persistent_values(), now);
  for (auto &other : session_registry_.sessions) {
    if (other.get() == &session || !other->automation_runtime ||
        other->source_key != session.source_key)
      continue;
    (void)session.sync_persistent_to(*other);
  }
}

} // namespace layerc::daemon
