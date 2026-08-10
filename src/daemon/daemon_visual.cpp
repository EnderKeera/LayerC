#include "daemon_app.hpp"

#include "layerc/platform/text.hpp"

#include <algorithm>

namespace layerc::daemon {
namespace {

constexpr std::size_t visual_quality_count = 3;
static_assert(static_cast<std::size_t>(
                  layerc::core::CaptureQuality::exact) +
                  1U ==
              visual_quality_count);

std::size_t quality_index(const layerc::core::CaptureQuality quality) {
  return static_cast<std::size_t>(quality);
}

layerc::core::CaptureQuality quality_from_index(const std::size_t index) {
  return static_cast<layerc::core::CaptureQuality>(index);
}

} // namespace

void DaemonApp::reconcile_visual_captures() {
  using QualityConfigurations =
      std::array<std::vector<VisualEvaluationRequest>, visual_quality_count>;
  std::unordered_map<HWND, QualityConfigurations> desired;
  if (visual_recognition_enabled_) {
    for (const auto *session : active_input_sessions_) {
      if (session->is_global() || !session->has_active_visual_rules() ||
          session->game_window == nullptr)
        continue;
      desired[session->game_window]
             [quality_index(session->visual_profile->quality)]
                 .push_back(
          {.source_key = session->source_key,
           .sample_rate_hz = session->visual_profile->sample_rate_hz,
           .quality = session->visual_profile->quality,
           .rules = session->visual_profile->rules});
    }
  }

  std::erase_if(visual_captures_, [&](auto &entry) {
    return !desired.contains(entry.first) || !IsWindow(entry.first);
  });
  for (const auto &[window, configurations] : desired) {
    auto &workers = visual_captures_[window];
    for (std::size_t index = 0; index < visual_quality_count; ++index) {
      if (configurations[index].empty()) {
        workers[index].reset();
        continue;
      }
      if (!workers[index]) {
        workers[index] = std::make_unique<VisualCaptureWorker>(
            window, quality_from_index(index));
        workers[index]->configure(configurations[index]);
        workers[index]->start();
      } else {
        workers[index]->configure(configurations[index]);
      }
    }
  }
  std::erase_if(visual_captures_, [](const auto &entry) {
    return std::ranges::none_of(
        entry.second, [](const auto &worker) { return worker != nullptr; });
  });

  if (!visual_captures_.empty() && !visual_timer_running_) {
    SetTimer(window_, visual_capture_timer, 33, nullptr);
    visual_timer_running_ = true;
  } else if (visual_captures_.empty() && visual_timer_running_) {
    KillTimer(window_, visual_capture_timer);
    visual_timer_running_ = false;
  }
  if (visual_capture_active_event_ != nullptr) {
    if (visual_captures_.empty())
      ResetEvent(visual_capture_active_event_);
    else
      SetEvent(visual_capture_active_event_);
  }
  update_visual_status();
}

void DaemonApp::poll_visual_captures() {
  const auto now = layerc::core::MonotonicClock::now();
  bool overlays_changed{};
  std::vector<VisualCaptureSnapshot> captured_snapshots;
  captured_snapshots.reserve(visual_captures_.size() * visual_quality_count);
  std::vector<RECT> exclusions;
  exclusions.reserve(16);
  for (auto &[window, workers] : visual_captures_) {
    exclusions.clear();
    for (const auto &session : sessions_) {
      if (session->is_global() || session->game_window != window) continue;
      for (const auto &overlay : session->overlays) {
        RECT bounds{};
        if (overlay->screen_bounds(bounds)) exclusions.push_back(bounds);
      }
    }
    for (std::size_t index = 0; index < visual_quality_count; ++index) {
      auto &worker = workers[index];
      if (!worker) continue;
      worker->set_excluded_screen_rects(exclusions);
      const auto captured = worker->snapshot();
      if (captured.sequence != 0) {
        for (auto *session : active_input_sessions_) {
          if (session->game_window != window ||
              !session->has_active_visual_rules() ||
              quality_index(session->visual_profile->quality) != index)
            continue;
          const auto evaluation = std::ranges::find(
              captured.evaluations, session->source_key,
              &VisualEvaluationSnapshot::source_key);
          if (evaluation == captured.evaluations.end() ||
              evaluation->sequence == session->last_visual_sequence)
            continue;
          session->last_visual_sequence = evaluation->sequence;

          session->cooldowns.snapshot(now, session->snapshot_buffer);
          const auto requests = session->visual_triggers->on_measurements(
              evaluation->measurements, session->snapshot_buffer, now);
          for (const auto &request : requests) {
            if (session->cooldowns.trigger(request.cooldown_id, now)) {
              session->visual_triggers->on_cooldown_triggered(
                  request.cooldown_id);
              overlays_changed = true;
              update_tooltip(
                  L"LayerD - visual trigger " +
                  layerc::platform::utf8_to_wide(request.cooldown_id));
            }
          }
        }
      }
      // Status should describe the newest capture result, not necessarily the
      // pre-evaluation snapshot used for this UI-thread polling pass.
      captured_snapshots.push_back(worker->snapshot());
    }
  }
  if (overlays_changed) update_overlays();
  publish_visual_status(captured_snapshots);
}

void DaemonApp::update_visual_status() {
  std::vector<VisualCaptureSnapshot> snapshots;
  snapshots.reserve(visual_captures_.size() * visual_quality_count);
  for (const auto &[window, workers] : visual_captures_) {
    (void)window;
    for (const auto &worker : workers) {
      if (worker) snapshots.push_back(worker->snapshot());
    }
  }
  publish_visual_status(snapshots);
}

void DaemonApp::publish_visual_status(
    const std::span<const VisualCaptureSnapshot> snapshots) {
  std::wstring next;
  const bool invalid = std::ranges::any_of(
      active_input_sessions_, [](const Session *session) {
        return session->visual_health ==
               layerc::platform::VisualSidecarState::invalid;
      });
  if (invalid) {
    next = L"Visual recognition: blocked (invalid sidecar)";
  } else if (!visual_recognition_enabled_) {
    next = L"Visual recognition: disabled";
  } else if (visual_captures_.empty()) {
    next = L"Visual recognition: inactive";
  } else {
    bool fallback{};
    bool unavailable{};
    std::wstring gpu;
    for (const auto &current : snapshots) {
      fallback = fallback ||
                 current.result.backend ==
                     layerc::core::CaptureBackendKind::desktop_duplication;
      unavailable = unavailable ||
                    current.result.availability ==
                        layerc::core::CaptureAvailability::unavailable ||
                    current.result.availability ==
                        layerc::core::CaptureAvailability::protected_or_black;
      if (gpu.empty() && !current.gpu_description.empty())
        gpu = layerc::platform::utf8_to_wide(current.gpu_description);
    }
    if (unavailable)
      next = L"Visual recognition: capture unavailable";
    else if (fallback)
      next = L"Visual recognition: Desktop Duplication fallback";
    else
      next = L"Visual recognition: WGC active";
    if (!gpu.empty()) next += L" - " + gpu;
  }
  if (next == visual_status_) return;
  visual_status_ = std::move(next);
  if (live_channel_.connected())
    (void)live_channel_.send({
        .type = layerc::core::LiveMessageType::runtime_status,
        .text = layerc::platform::wide_to_utf8(visual_status_),
    });
}

} // namespace layerc::daemon
