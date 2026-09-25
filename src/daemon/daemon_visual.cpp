#include "daemon_app.hpp"

#include "layerc/platform/text.hpp"
#include "layerc/platform/visual_assets_windows.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace layerc::daemon {
namespace {

bool observation_enabled(const layerc::core::VisualProfile &profile,
                         const std::string_view id) {
  return std::ranges::any_of(profile.triggers, [&](const auto &trigger) {
    return trigger.enabled && trigger.observation_id == id;
  });
}

VisualEvaluationRequest build_visual_request(const Session &session) {
  const auto &visual = *session.modules.visual.profile;
  VisualEvaluationRequest request;
  request.source_key = session.source_key;
  request.sample_rate_hz = visual.sample_rate_hz;
  request.quality = visual.quality;
  for (const auto &rule : visual.rules) {
    if (observation_enabled(visual, rule.id))
      request.rules.push_back(rule);
  }
  for (const auto &state : visual.templates) {
    if (!observation_enabled(visual, state.id))
      continue;
    VisualTemplateEvaluation evaluation;
    evaluation.state = state;
    evaluation.asset_signatures.reserve(state.references.size());
    for (const auto &reference : state.references) {
      evaluation.asset_signatures.push_back(
          layerc::platform::asset_signature(reference.asset_sha256));
    }
    request.templates.push_back(std::move(evaluation));
  }
  return request;
}

} // namespace

void DaemonApp::reconcile_visual_captures(
    const layerc::runtime::CapabilityDemand &demand,
    const layerc::core::VisualRuntime runtime) {
  std::unordered_map<HWND, std::vector<VisualEvaluationRequest>> desired;
  for (const auto &consumer : demand.visual_consumers()) {
    if (consumer.runtime != runtime) continue;
    const auto found = std::ranges::find_if(session_registry_.sessions, [&](const auto &session) {
      return session != nullptr && session->profile.id == consumer.profile_id &&
             reinterpret_cast<std::uintptr_t>(session->game_window) ==
                 consumer.window.opaque;
    });
    if (found == session_registry_.sessions.end() || !(*found)->modules.visual.profile)
      continue;
    const auto *session = found->get();
    desired[session->game_window].push_back(build_visual_request(*session));
  }

  if (!desired.empty())
    (void)visual_capture_runtime_.ensure_snapshot_event();
  std::erase_if(visual_capture_runtime_.captures, [&](auto &entry) {
    if (entry.second == nullptr || !IsWindow(entry.first)) return true;
    const auto wanted = desired.find(entry.first);
    if (wanted != desired.end()) return entry.second->runtime() != runtime;
    return entry.second->runtime() == runtime;
  });
  for (auto &[window, requests] : desired) {
    if (requests.empty())
      continue;
    const auto quality =
        std::ranges::max(requests, {}, &VisualEvaluationRequest::quality)
            .quality;
    auto &worker = visual_capture_runtime_.captures[window];
    const bool created = worker == nullptr;
    if (created) {
      if (runtime == layerc::core::VisualRuntime::isolated)
        worker = std::make_unique<IsolatedVisualWorker>(
            window, visual_capture_runtime_.snapshot_event());
      else
        worker =
            std::make_unique<VisualCaptureWorker>(window, visual_capture_runtime_.snapshot_event());
    }
    worker->configure(std::move(requests), quality);
    if (created)
      worker->start();
  }
  sync_visual_exclusions();

  const bool any_capture = !visual_capture_runtime_.captures.empty();
  if (!any_capture)
    visual_capture_runtime_.close_snapshot_event();
  visual_capture_runtime_.publish_activity(any_capture);
  update_visual_status();
}

void DaemonApp::quiesce_visual_captures(
    const layerc::core::VisualRuntime runtime) noexcept {
  std::erase_if(visual_capture_runtime_.captures, [runtime](const auto &entry) {
    return entry.second == nullptr || entry.second->runtime() == runtime;
  });
  if (visual_capture_runtime_.captures.empty())
    visual_capture_runtime_.close_snapshot_event();
  visual_capture_runtime_.publish_activity(
      !visual_capture_runtime_.captures.empty());
  update_visual_status();
}

void DaemonApp::sync_visual_exclusions() {
  for (auto &[window, worker] : visual_capture_runtime_.captures) {
    if (worker == nullptr)
      continue;
    std::vector<RECT> exclusions;
    exclusions.reserve(16);
    for (const auto &session : session_registry_.sessions) {
      if (session->is_global() || session->game_window != window)
        continue;
      session->overlays.append_screen_bounds(exclusions);
    }
    // Hosts compare rectangles and ignore identical pushes.
    worker->set_excluded_screen_rects(exclusions);
  }
}

void DaemonApp::poll_visual_captures() {
  const auto now = layerc::core::MonotonicClock::now();
  std::vector<VisualCaptureSnapshot> captured_snapshots;
  captured_snapshots.reserve(visual_capture_runtime_.captures.size());
  const auto poll_capture = [&](const HWND window, auto &worker) {
    const auto captured = worker.snapshot();
    if (captured.sequence != 0) {
      for (auto *session : session_registry_.active_inputs) {
        if (session->game_window != window ||
            !session->has_active_visual_rules())
          continue;
        const auto evaluation =
            std::ranges::find(captured.evaluations, session->source_key,
                              &VisualEvaluationSnapshot::source_key);
        if (evaluation == captured.evaluations.end() ||
            evaluation->sequence == session->last_visual_sequence)
          continue;
        session->last_visual_sequence = evaluation->sequence;
        const auto result = session->layerc::core::AutomationSession::
            feed_measurements(evaluation->measurements, session->profile, now);
        if (result.changed)
          mark_overlays_dirty();
        if (!result.triggered.empty())
          update_tooltip(L"LayerD - visual trigger " +
                         layerc::platform::utf8_to_wide(result.triggered.back()));
        if (session->automation_runtime) {
          publish_automation_trace(*session);
          synchronize_automation_state(*session, now);
          schedule_automation_timer();
        }
      }
    }
    captured_snapshots.push_back(captured);
  };
  for (auto &[window, worker] : visual_capture_runtime_.captures) {
    if (worker)
      poll_capture(window, *worker);
  }
  publish_visual_status(captured_snapshots);
}

void DaemonApp::maintain_visual_captures() {
  for (auto &[window, worker] : visual_capture_runtime_.captures) {
    (void)window;
    if (worker)
      worker->maintain();
  }
  update_visual_status();
}

void DaemonApp::retry_quarantined_visual_captures() {
  for (auto &[window, worker] : visual_capture_runtime_.captures) {
    (void)window;
    if (worker && worker->status().quarantined)
      worker->retry();
  }
  update_visual_status();
}

void DaemonApp::update_visual_status() {
  std::vector<VisualCaptureSnapshot> snapshots;
  snapshots.reserve(visual_capture_runtime_.captures.size());
  for (const auto &[window, worker] : visual_capture_runtime_.captures) {
    (void)window;
    if (worker)
      snapshots.push_back(worker->snapshot());
  }
  publish_visual_status(snapshots);
}

void DaemonApp::publish_visual_status(
    const std::span<const VisualCaptureSnapshot> snapshots) {
  VisualStatusSummary summary;
  summary.invalid_sidecar = std::ranges::any_of(
      session_registry_.active_inputs, [](const Session *session) {
        return session->modules.visual.state ==
               layerc::platform::ModuleSidecarState::invalid;
      });
  summary.disabled = !visual_recognition_enabled_;
  summary.inactive = visual_capture_runtime_.captures.empty();
  for (const auto &current : snapshots) {
    summary.fallback =
        summary.fallback ||
        current.result.backend ==
            layerc::core::CaptureBackendKind::desktop_duplication;
    summary.unavailable =
        summary.unavailable ||
        current.result.availability ==
            layerc::core::CaptureAvailability::unavailable ||
        current.result.availability ==
            layerc::core::CaptureAvailability::protected_or_black;
    summary.missing_assets =
        summary.missing_assets ||
        std::ranges::any_of(current.evaluations, [](const auto &evaluation) {
          return evaluation.missing_template_assets;
        });
    if (summary.gpu.empty() && !current.gpu_description.empty())
      summary.gpu = current.gpu_description;
  }
  for (const auto &[window, worker] : visual_capture_runtime_.captures) {
    (void)window;
    if (worker)
      summary.quarantined = summary.quarantined || worker->status().quarantined;
  }

  auto next = format_visual_status(summary);
  if (next == visual_capture_runtime_.status)
    return;
  visual_capture_runtime_.status = std::move(next);
  if (live_edit_host_.channel.connected() && live_edit_host_.authenticated)
    (void)live_edit_host_.channel.send({
        .type = layerc::core::LiveMessageType::runtime_status,
        .text = layerc::platform::wide_to_utf8(visual_capture_runtime_.status),
    });
}

std::wstring format_visual_status(const VisualStatusSummary &summary) {
  std::wstring next;
  if (summary.invalid_sidecar) {
    next = L"Visual recognition: blocked (invalid sidecar)";
  } else if (summary.disabled) {
    next = L"Visual recognition: disabled";
  } else if (summary.inactive) {
    next = L"Visual recognition: inactive";
  } else if (summary.quarantined) {
    next = L"Visual recognition: isolated helper quarantined (use Runtime "
           L"Diagnostics to retry)";
  } else if (summary.missing_assets) {
    next = L"Visual recognition: template asset missing or corrupt";
  } else if (summary.unavailable) {
    next = L"Visual recognition: capture unavailable";
  } else if (summary.fallback) {
    next = L"Visual recognition: Desktop Duplication fallback";
  } else {
    next = L"Visual recognition: WGC active";
  }
  if (!summary.gpu.empty())
    next += L" - " + layerc::platform::utf8_to_wide(summary.gpu);
  return next;
}

} // namespace layerc::daemon
