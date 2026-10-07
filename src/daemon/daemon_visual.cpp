#include "daemon_app.hpp"

#include "layerc/platform/text.hpp"
#include "layerc/platform/visual_assets_windows.hpp"

#include <algorithm>
#include <string>
#include <utility>
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
  const auto &visual = *session.modules.get<layerc::core::VisualModule>().profile;
  VisualEvaluationRequest request;
  request.source_key = session.source_key;
  request.sample_rate_hz = visual.sample_rate_hz;
  request.quality = visual.quality;
  for (const auto &rule :
       layerc::core::observations_of<layerc::core::PixelRule>(
           visual.observations)) {
    if (observation_enabled(visual, rule.id))
      request.rules.push_back(rule);
  }
  for (const auto &state :
       layerc::core::observations_of<layerc::core::TemplateState>(
           visual.observations)) {
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

layerc::runtime::ServiceReconcileResult VisualCaptureService::reconcile(
    const layerc::runtime::CapabilityDemand &demand) {
  const auto runtime = runtime_;
  auto &sessions = captures_.sessions();
  std::unordered_map<HWND, std::vector<VisualEvaluationRequest>> desired;
  for (const auto &consumer : demand.visual_consumers()) {
    if (consumer.runtime != runtime) continue;
    const auto found = std::ranges::find_if(sessions.sessions, [&](const auto &session) {
      return session != nullptr && session->automation.profile().id == consumer.profile_id &&
             reinterpret_cast<std::uintptr_t>(session->game_window) ==
                 consumer.window.opaque;
    });
    if (found == sessions.sessions.end() || !(*found)->modules.get<layerc::core::VisualModule>().profile)
      continue;
    const auto *session = found->get();
    desired[session->game_window].push_back(build_visual_request(*session));
  }

  if (!desired.empty())
    (void)captures_.ensure_snapshot_event();
  std::erase_if(captures_.captures, [&](auto &entry) {
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
    auto &worker = captures_.captures[window];
    const bool created = worker == nullptr;
    if (created) {
      if (runtime == layerc::core::VisualRuntime::isolated)
        worker = std::make_unique<IsolatedVisualWorker>(
            window, captures_.snapshot_event());
      else
        worker =
            std::make_unique<VisualCaptureWorker>(window, captures_.snapshot_event());
    }
    worker->configure(std::move(requests), quality);
    if (created)
      worker->start();
  }
  captures_.sync_exclusions();

  const bool any_capture = !captures_.captures.empty();
  if (!any_capture)
    captures_.close_snapshot_event();
  captures_.publish_activity(any_capture);
  captures_.update_status();
  return {};
}

void VisualCaptureService::quiesce() noexcept {
  const auto runtime = runtime_;
  std::erase_if(captures_.captures, [runtime](const auto &entry) {
    return entry.second == nullptr || entry.second->runtime() == runtime;
  });
  if (captures_.captures.empty())
    captures_.close_snapshot_event();
  captures_.publish_activity(
      !captures_.captures.empty());
  captures_.update_status();
}

void VisualCaptureRuntime::sync_exclusions() {
  for (auto &[window, worker] : captures) {
    if (worker == nullptr)
      continue;
    std::vector<RECT> exclusions;
    exclusions.reserve(16);
    for (const auto &session : sessions_.sessions) {
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
    auto captured = worker.snapshot();
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
        const auto result = session->automation.feed_measurements(evaluation->measurements, now);
        if (result.changed)
          overlay_runtime_.mark_dirty();
        if (!result.triggered.empty())
          update_tooltip(L"LayerD - visual trigger " +
                         layerc::platform::utf8_to_wide(result.triggered.back()));
        if (session->automation.runtime())
          automation_runtime_host_.after_step(*session, now);
      }
    }
    captured_snapshots.push_back(std::move(captured));
  };
  for (auto &[window, worker] : visual_capture_runtime_.captures) {
    if (worker)
      poll_capture(window, *worker);
  }
  visual_capture_runtime_.publish_status(captured_snapshots);
}

void DaemonApp::maintain_visual_captures() {
  for (auto &[window, worker] : visual_capture_runtime_.captures) {
    (void)window;
    if (worker)
      worker->maintain();
  }
  visual_capture_runtime_.update_status();
}

void DaemonApp::retry_quarantined_visual_captures() {
  for (auto &[window, worker] : visual_capture_runtime_.captures) {
    (void)window;
    if (worker && worker->status().quarantined)
      worker->retry();
  }
  visual_capture_runtime_.update_status();
}

void VisualCaptureRuntime::update_status() {
  std::vector<VisualCaptureSnapshot> snapshots;
  snapshots.reserve(captures.size());
  for (const auto &[window, worker] : captures) {
    (void)window;
    if (worker)
      snapshots.push_back(worker->snapshot());
  }
  publish_status(snapshots);
}

void VisualCaptureRuntime::publish_status(
    const std::span<const VisualCaptureSnapshot> snapshots) {
  VisualStatusSummary summary;
  summary.invalid_sidecar = std::ranges::any_of(
      sessions_.active_inputs, [](const Session *session) {
        return session->modules.get<layerc::core::VisualModule>().state ==
               layerc::platform::ModuleSidecarState::invalid;
      });
  summary.disabled = !recognition_enabled;
  summary.inactive = captures.empty();
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
  for (const auto &[window, worker] : captures) {
    (void)window;
    if (worker)
      summary.quarantined = summary.quarantined || worker->status().quarantined;
  }

  auto next = format_visual_status(summary);
  if (next == status)
    return;
  status = std::move(next);
  if (live_edit_.channel.connected() && live_edit_.authenticated)
    (void)live_edit_.channel.send({
        .type = layerc::core::LiveMessageType::runtime_status,
        .text = layerc::platform::wide_to_utf8(status),
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
