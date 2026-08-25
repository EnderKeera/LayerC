#include "daemon_app.hpp"

#include "layerc/platform/text.hpp"
#include "layerc/platform/visual_assets.hpp"

#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>

namespace layerc::daemon {
namespace {

bool observation_enabled(const layerc::core::VisualProfile &profile,
                         const std::string_view id) {
  return std::ranges::any_of(profile.triggers, [&](const auto &trigger) {
    return trigger.enabled && trigger.observation_id == id;
  });
}

VisualAssetSignature asset_signature(const std::string_view hash) {
  const auto path = layerc::platform::visual_asset_path(hash);
  if (path.empty())
    return {};
  const HANDLE file =
      CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return {};
  BY_HANDLE_FILE_INFORMATION information{};
  const bool loaded = GetFileInformationByHandle(file, &information) != FALSE;
  CloseHandle(file);
  if (!loaded)
    return {};
  const auto combine = [](const DWORD high, const DWORD low) {
    return (static_cast<std::uint64_t>(high) << 32U) | low;
  };
  // The change-journal USN binds the signature to content history: any
  // write bumps it, so a replacement that preserves size and mtime — and
  // even one whose MFT record reuses the same file index — still produces a
  // different signature and forces the worker to re-decode. Volumes without
  // a journal keep 0 and fall back to the metadata fields.
  std::uint64_t change_usn{};
  std::array<unsigned char, 128> journal{};
  DWORD journal_bytes{};
  if (DeviceIoControl(file, FSCTL_READ_FILE_USN_DATA, nullptr, 0,
                      journal.data(), static_cast<DWORD>(journal.size()),
                      &journal_bytes, nullptr) &&
      journal_bytes >= sizeof(DWORD) + sizeof(WORD)) {
    WORD version{};
    std::memcpy(&version, journal.data() + sizeof(DWORD), sizeof(version));
    std::size_t usn_offset = std::size_t(-1);
    // V2: RecordLength(4) + versions(4) + two 8-byte file IDs = 24.
    // V3 replaces those file IDs with two 16-byte IDs, moving Usn to 40.
    // USN_RECORD is the V2-compatible alias; spell V3 manually for MinGW.
    static_assert(offsetof(USN_RECORD, Usn) == 24,
                  "USN_RECORD Usn offset changed — update V3 manual offset");
    if (version == 2)
      usn_offset = offsetof(USN_RECORD, Usn);
    else if (version == 3)
      usn_offset = sizeof(DWORD) + 2U * sizeof(WORD) +
                   2U * 2U * sizeof(std::uint64_t); // 4+4+32 = 40
    if (usn_offset != std::size_t(-1) &&
        journal_bytes >= usn_offset + sizeof(change_usn))
      std::memcpy(&change_usn, journal.data() + usn_offset, sizeof(change_usn));
  }
  return {
      .present = true,
      .volume_serial = information.dwVolumeSerialNumber,
      .file_index =
          combine(information.nFileIndexHigh, information.nFileIndexLow),
      .file_size = combine(information.nFileSizeHigh, information.nFileSizeLow),
      .last_write_ticks = combine(information.ftLastWriteTime.dwHighDateTime,
                                  information.ftLastWriteTime.dwLowDateTime),
      .change_usn = change_usn};
}

} // namespace

void DaemonApp::reconcile_visual_captures() {
  std::unordered_map<HWND, std::vector<VisualEvaluationRequest>> desired;
  std::unordered_map<HWND, bool> isolated;
  if (visual_recognition_enabled_) {
    for (const auto *session : active_input_sessions_) {
      if (session->is_global() || !session->has_active_visual_rules() ||
          session->game_window == nullptr)
        continue;
      VisualEvaluationRequest request;
      request.source_key = session->source_key;
      request.sample_rate_hz = session->visual_profile->sample_rate_hz;
      request.quality = session->visual_profile->quality;
      for (const auto &rule : session->visual_profile->rules) {
        if (observation_enabled(*session->visual_profile, rule.id))
          request.rules.push_back(rule);
      }
      for (const auto &state : session->visual_profile->templates) {
        if (!observation_enabled(*session->visual_profile, state.id))
          continue;
        VisualTemplateEvaluation evaluation;
        evaluation.state = state;
        evaluation.asset_signatures.reserve(state.references.size());
        for (const auto &reference : state.references) {
          evaluation.asset_signatures.push_back(
              asset_signature(reference.asset_sha256));
        }
        request.templates.push_back(std::move(evaluation));
      }
      desired[session->game_window].push_back(std::move(request));
      auto &window_isolated = isolated[session->game_window];
      window_isolated =
          window_isolated || session->profile.visual_runtime ==
                                 layerc::core::VisualRuntime::isolated;
    }
  }

  if (!desired.empty() && visual_snapshot_event_ == nullptr)
    visual_snapshot_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  const auto exclusions_for = [&](const HWND window) {
    std::vector<RECT> result;
    result.reserve(16);
    for (const auto &session : sessions_) {
      if (session->is_global() || session->game_window != window)
        continue;
      session->overlays.append_screen_bounds(result);
    }
    return result;
  };
  const auto is_isolated = [&](const HWND window) {
    const auto found = isolated.find(window);
    return found != isolated.end() && found->second;
  };

  std::erase_if(visual_captures_, [&](auto &entry) {
    return !desired.contains(entry.first) || is_isolated(entry.first) ||
           !IsWindow(entry.first);
  });
  std::erase_if(isolated_visual_captures_, [&](auto &entry) {
    return !desired.contains(entry.first) || !is_isolated(entry.first) ||
           !IsWindow(entry.first);
  });
  for (auto &[window, configurations] : desired) {
    if (configurations.empty())
      continue;
    const auto quality =
        std::ranges::max(configurations, {}, &VisualEvaluationRequest::quality)
            .quality;
    if (is_isolated(window)) {
      visual_captures_.erase(window);
      auto &worker = isolated_visual_captures_[window];
      if (!worker) {
        worker = std::make_unique<IsolatedVisualWorker>(window,
                                                        visual_snapshot_event_);
        worker->configure(configurations, quality);
        const auto exclusions = exclusions_for(window);
        worker->set_excluded_screen_rects(exclusions);
        worker->start();
      } else {
        worker->configure(configurations, quality);
      }
      continue;
    }
    isolated_visual_captures_.erase(window);
    auto &worker = visual_captures_[window];
    if (!worker || worker->quality() != quality) {
      worker = std::make_unique<VisualCaptureWorker>(window, quality,
                                                     visual_snapshot_event_);
      worker->configure(std::move(configurations));
      const auto exclusions = exclusions_for(window);
      worker->set_excluded_screen_rects(exclusions);
      worker->start();
    } else {
      worker->configure(std::move(configurations));
    }
  }

  const bool any_capture =
      !visual_captures_.empty() || !isolated_visual_captures_.empty();
  if (!any_capture && visual_snapshot_event_ != nullptr) {
    CloseHandle(visual_snapshot_event_);
    visual_snapshot_event_ = nullptr;
  }
  if (visual_capture_active_event_ != nullptr) {
    if (!any_capture)
      ResetEvent(visual_capture_active_event_);
    else
      SetEvent(visual_capture_active_event_);
  }
  update_visual_status();
}

void DaemonApp::quiesce_inline_visual_captures() noexcept {
  visual_captures_.clear();
  if (isolated_visual_captures_.empty() && visual_snapshot_event_ != nullptr) {
    CloseHandle(visual_snapshot_event_);
    visual_snapshot_event_ = nullptr;
  }
  if (visual_capture_active_event_ != nullptr &&
      isolated_visual_captures_.empty())
    ResetEvent(visual_capture_active_event_);
  update_visual_status();
}

void DaemonApp::quiesce_isolated_visual_captures() noexcept {
  isolated_visual_captures_.clear();
  if (visual_captures_.empty() && visual_snapshot_event_ != nullptr) {
    CloseHandle(visual_snapshot_event_);
    visual_snapshot_event_ = nullptr;
  }
  if (visual_capture_active_event_ != nullptr && visual_captures_.empty())
    ResetEvent(visual_capture_active_event_);
  update_visual_status();
}

void DaemonApp::quiesce_visual_captures() noexcept {
  visual_captures_.clear();
  isolated_visual_captures_.clear();
  if (visual_snapshot_event_ != nullptr)
    CloseHandle(visual_snapshot_event_);
  visual_snapshot_event_ = nullptr;
  if (visual_capture_active_event_ != nullptr)
    ResetEvent(visual_capture_active_event_);
  update_visual_status();
}

void DaemonApp::poll_visual_captures() {
  const auto now = layerc::core::MonotonicClock::now();
  bool overlays_changed{};
  std::vector<VisualCaptureSnapshot> captured_snapshots;
  captured_snapshots.reserve(visual_captures_.size());
  std::vector<RECT> exclusions;
  exclusions.reserve(16);
  const auto publish_observations =
      [&](Session &session,
          const std::span<const layerc::core::ObservationMeasurement>
              measurements) {
        bool changed{};
        if (!session.automation_runtime)
          return changed;
        for (const auto &measurement : measurements) {
          auto event = automation_event(
              layerc::core::AutomationEventKind::visual_observation,
              measurement.observation_id, now);
          event.fields.emplace(
              "available", layerc::core::AutomationValue(
                               measurement.availability ==
                               layerc::core::CaptureAvailability::available));
          event.fields.emplace(
              "matched", layerc::core::AutomationValue(measurement.matched));
          event.fields.emplace("confidence", layerc::core::AutomationValue(
                                                 layerc::core::ConfidenceValue{
                                                     measurement.confidence}));
          changed = process_automation_event(session, event) || changed;
        }
        return changed;
      };
  for (auto &[window, worker] : visual_captures_) {
    exclusions.clear();
    for (const auto &session : sessions_) {
      if (session->is_global() || session->game_window != window)
        continue;
      session->overlays.append_screen_bounds(exclusions);
    }
    if (!worker)
      continue;
    worker->set_excluded_screen_rects(exclusions);
    const auto captured = worker->snapshot();
    if (captured.sequence != 0) {
      for (auto *session : active_input_sessions_) {
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

        session->cooldowns.snapshot(now, session->snapshot_buffer);
        overlays_changed =
            publish_observations(*session, evaluation->measurements) ||
            overlays_changed;
        const auto requests = session->visual_triggers->on_measurements(
            evaluation->measurements, session->snapshot_buffer, now);
        for (const auto &request : requests) {
          if (trigger_cooldown_with_automation(*session, request.cooldown_id,
                                               now)) {
            overlays_changed = true;
            update_tooltip(L"LayerD - visual trigger " +
                           layerc::platform::utf8_to_wide(request.cooldown_id));
          }
        }
      }
    }
    captured_snapshots.push_back(captured);
  }
  for (auto &[window, worker] : isolated_visual_captures_) {
    exclusions.clear();
    for (const auto &session : sessions_) {
      if (session->is_global() || session->game_window != window)
        continue;
      session->overlays.append_screen_bounds(exclusions);
    }
    worker->set_excluded_screen_rects(exclusions);
    worker->maintain();
    const auto captured = worker->snapshot();
    if (captured.sequence != 0) {
      for (auto *session : active_input_sessions_) {
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
        session->cooldowns.snapshot(now, session->snapshot_buffer);
        overlays_changed =
            publish_observations(*session, evaluation->measurements) ||
            overlays_changed;
        const auto requests = session->visual_triggers->on_measurements(
            evaluation->measurements, session->snapshot_buffer, now);
        for (const auto &request : requests) {
          if (trigger_cooldown_with_automation(*session, request.cooldown_id,
                                               now)) {
            overlays_changed = true;
            update_tooltip(L"LayerD - visual trigger " +
                           layerc::platform::utf8_to_wide(request.cooldown_id));
          }
        }
      }
    }
    captured_snapshots.push_back(captured);
  }
  if (overlays_changed)
    update_overlays();
  publish_visual_status(captured_snapshots);
}

void DaemonApp::maintain_isolated_visual_captures() {
  for (auto &[window, worker] : isolated_visual_captures_) {
    (void)window;
    if (worker)
      worker->maintain();
  }
  update_visual_status();
}

void DaemonApp::retry_quarantined_visual_helpers() {
  for (auto &[window, worker] : isolated_visual_captures_) {
    (void)window;
    if (worker && worker->status().quarantined)
      worker->retry();
  }
  update_visual_status();
}

void DaemonApp::update_visual_status() {
  std::vector<VisualCaptureSnapshot> snapshots;
  snapshots.reserve(visual_captures_.size());
  for (const auto &[window, worker] : visual_captures_) {
    (void)window;
    if (worker)
      snapshots.push_back(worker->snapshot());
  }
  for (const auto &[window, worker] : isolated_visual_captures_) {
    (void)window;
    if (worker)
      snapshots.push_back(worker->snapshot());
  }
  publish_visual_status(snapshots);
}

void DaemonApp::publish_visual_status(
    const std::span<const VisualCaptureSnapshot> snapshots) {
  std::wstring next;
  const bool invalid =
      std::ranges::any_of(active_input_sessions_, [](const Session *session) {
        return session->visual_health ==
               layerc::platform::VisualSidecarState::invalid;
      });
  if (invalid) {
    next = L"Visual recognition: blocked (invalid sidecar)";
  } else if (!visual_recognition_enabled_) {
    next = L"Visual recognition: disabled";
  } else if (visual_captures_.empty() && isolated_visual_captures_.empty()) {
    next = L"Visual recognition: inactive";
  } else {
    bool fallback{};
    bool unavailable{};
    bool missing_assets{};
    bool quarantined{};
    std::wstring gpu;
    for (const auto &current : snapshots) {
      fallback =
          fallback || current.result.backend ==
                          layerc::core::CaptureBackendKind::desktop_duplication;
      unavailable = unavailable ||
                    current.result.availability ==
                        layerc::core::CaptureAvailability::unavailable ||
                    current.result.availability ==
                        layerc::core::CaptureAvailability::protected_or_black;
      missing_assets =
          missing_assets ||
          std::ranges::any_of(current.evaluations, [](const auto &evaluation) {
            return evaluation.missing_template_assets;
          });
      if (gpu.empty() && !current.gpu_description.empty())
        gpu = layerc::platform::utf8_to_wide(current.gpu_description);
    }
    for (const auto &[window, worker] : isolated_visual_captures_) {
      (void)window;
      quarantined = quarantined || worker->status().quarantined;
    }
    if (quarantined)
      next = L"Visual recognition: isolated helper quarantined (use Runtime "
             L"Diagnostics to retry)";
    else if (missing_assets)
      next = L"Visual recognition: template asset missing or corrupt";
    else if (unavailable)
      next = L"Visual recognition: capture unavailable";
    else if (fallback)
      next = L"Visual recognition: Desktop Duplication fallback";
    else
      next = L"Visual recognition: WGC active";
    if (!gpu.empty())
      next += L" - " + gpu;
  }
  if (next == visual_status_)
    return;
  visual_status_ = std::move(next);
  if (live_channel_.connected() && live_package_authenticated_)
    (void)live_channel_.send({
        .type = layerc::core::LiveMessageType::runtime_status,
        .text = layerc::platform::wide_to_utf8(visual_status_),
    });
}

} // namespace layerc::daemon
