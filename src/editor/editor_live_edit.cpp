#include "editor_window.hpp"
#include "automation_editor.hpp"
#include "editor_resource.h"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <variant>

#include <commctrl.h>
#include <windows.h>

namespace layerc::editor {
namespace {

// Do not publish an in-memory schema-v3 migration before the user has
// approved making that one-way disk upgrade.
bool live_publish_allowed(const ProfileDocument &document) {
  return (!document.legacy_base_schema() && !document.legacy_visual_schema()) ||
         document.upgrade_acknowledged();
}

} // namespace

void EditorWindow::sync_current_profile_entry() {
  if (document_.path().empty() || shell_.profile_list == nullptr) return;
  const auto current_key =
      layerc::platform::canonical_profile_key(document_.path());
  for (std::size_t index = 0; index < shell_.profiles.size(); ++index) {
    auto &entry = shell_.profiles[index];
    if (layerc::platform::canonical_profile_key(entry.path) != current_key)
      continue;
    entry.describe(document_.profile());
    auto sync = shell_.sync_scope();
    ListView_SetItemText(shell_.profile_list, static_cast<int>(index), 0,
                         entry.label.data());
    ListView_SetCheckState(shell_.profile_list, static_cast<int>(index),
                           entry.enabled);
    InvalidateRect(shell_.profile_list, nullptr, TRUE);
    return;
  }
}

void EditorWindow::document_changed(const DocumentChangeKind kind) {
  schedule_autosave(kind != DocumentChangeKind::text);
  if (live_publish_allowed(document_)) send_live_profile();
  sync_current_profile_entry();
  refresh_title();
}

bool EditorWindow::apply_edit(const ModuleEditOutcome &outcome,
                              const DocumentChangeKind kind) {
  if (outcome.accepted()) {
    document_changed(kind);
    return true;
  }
  if (outcome.status == ModuleEditStatus::unchanged) {
    // An invalid buffer keeps today's autosave pause.
    if (!draft_controls_valid()) schedule_autosave(false);
    return true;
  }
  set_status(to_wide(outcome.message));
  return false;
}

bool EditorWindow::apply_profile_change(
    const DocumentChangeKind kind,
    const std::function<void(layerc::core::GameProfile &)> &extra) {
  if (shell_.syncing) return true;
  const auto cooldown_index = cooldown_editor_.selected_index();
  const auto panel_index = panel_preview_.controls_panel;
  const auto outcome = document_.edit_profile(
      [&](layerc::core::GameProfile &candidate) {
        auto *panel = panel_index < candidate.panels.size()
                          ? &candidate.panels[panel_index]
                          : nullptr;
        auto *cooldown =
            cooldown_index && *cooldown_index < candidate.cooldowns.size()
                ? &candidate.cooldowns[*cooldown_index]
                : nullptr;
        ProfileFieldContext context{shell_.window, candidate, cooldown, panel};
        read_profile_fields(context);
        if (extra) extra(candidate);
      },
      kind);
  return apply_edit(outcome, kind);
}

bool EditorWindow::apply_profile_controls(const DocumentChangeKind kind) {
  return apply_profile_change(kind, {});
}

bool EditorWindow::has_unsaved_changes() const {
  return document_.dirty() || !draft_controls_valid();
}

void EditorWindow::schedule_autosave(const bool immediate) {
  if (document_.path().empty()) {
    set_status(L"Draft changed — use Save once to create the profile.",
               StatusVoice::silent);
    return;
  }
  KillTimer(shell_.window, autosave_timer);
  SetTimer(shell_.window, autosave_timer, immediate ? 1U : 400U, nullptr);
  if (!immediate)
    set_status(L"Editing — autosave pending.", StatusVoice::silent);
}

bool EditorWindow::draft_controls_valid() const {
  // F01 amendment (lead-approved): a valid buffer is per-field valid AND fully
  // applied, i.e. reading the controls into a copy of the document changes
  // nothing. The document itself may be legitimately incomplete (an untouched
  // New Profile has no executable); only pending buffer content is invalid.
  // With no controls bound (headless fixtures) reading changes nothing.
  auto candidate = document_.profile();
  const auto panel_index = panel_preview_.controls_panel;
  auto *panel = panel_index < candidate.panels.size()
                    ? &candidate.panels[panel_index]
                    : nullptr;
  const auto cooldown_index = cooldown_editor_.selected_index();
  auto *cooldown = cooldown_index && *cooldown_index < candidate.cooldowns.size()
                       ? &candidate.cooldowns[*cooldown_index]
                       : nullptr;
  ProfileFieldContext context{shell_.window, candidate, cooldown, panel};
  if (!profile_fields_valid(context)) return false;
  read_profile_fields(context);
  return candidate == document_.profile();
}

bool EditorWindow::require_stored_profile(const wchar_t *const first_save_message) {
  if (document_.path().empty()) {
    themed_message_box(shell_.window, first_save_message, L"Create profile first",
                MB_OK | MB_ICONINFORMATION);
    return false;
  }
  return !has_unsaved_changes() || flush_autosave();
}

bool EditorWindow::flush_autosave(const bool allow_upgrade_prompt) {
  KillTimer(shell_.window, autosave_timer);
  document_.end_text_coalescing();
  if (!has_unsaved_changes()) return true;
  if (document_.path().empty() || !draft_controls_valid()) {
    set_status(document_.path().empty()
                   ? L"Draft is not created yet — use Save."
                   : L"Not autosaved — finish the invalid field.",
               StatusVoice::changed);
    return false;
  }
  switch (commit_document(document_.path(),
                        {.prompt_upgrade = allow_upgrade_prompt})) {
  case CommitResult::ok:
    mark_committed();
    // Autosave follows every edit: its success stays quiet, and a failure
    // is spoken once while its cause stays.
    set_status(L"Autosaved — LayerD updated live.", StatusVoice::silent);
    return true;
  case CommitResult::invalid_fields:
    set_status(L"Not autosaved — finish the invalid field.",
               StatusVoice::changed);
    break;
  case CommitResult::invalid_profile:
    set_status(L"Not autosaved — profile validation failed.",
               StatusVoice::changed);
    break;
  case CommitResult::upgrade_declined:
    set_status(allow_upgrade_prompt
                   ? L"Not saved — the profile format upgrade was not confirmed."
                   : L"Autosave paused — use Save to approve the profile format upgrade.",
               StatusVoice::changed);
    break;
  case CommitResult::blocked_by_sidecar:
    set_status(L"Not autosaved — repair the invalid visual or automation settings first.",
               StatusVoice::changed);
    break;
  case CommitResult::write_failed:
    set_status(write_failure_message(), StatusVoice::changed);
    break;
  }
  return false;
}

void EditorWindow::notify_profile_committed(
    const std::filesystem::path &path,
    const layerc::core::GameProfile &profile) {
  if (!live_edit_.publish_committed(
          layerc::platform::canonical_profile_key(path),
          layerc::core::serialize_profile_json(profile))) {
    (void)start_daemon();
    (void)layerc::platform::signal_profile_reload();
  }
}

void EditorWindow::poll_live_channel() {
  const auto step = live_edit_.poll([this](const std::string_view panel_id) {
    return std::ranges::any_of(document_.profile().panels,
                               [&](const auto &panel) {
                                 return panel.id == panel_id;
                               });
  });
  for (const auto &error : step.protocol_errors)
    set_status(L"Live channel error: " + to_wide(error.message),
               StatusVoice::changed);
  for (const auto &message : step.forwarded) handle_live_message(message);
  if (step.authenticating)
    set_status(L"Authenticating LayerD package...", StatusVoice::silent);
  if (step.authentication_rejected)
    set_status(L"Connected LayerD is not from the current LayerC package.");
  if (step.authenticated_now) {
    set_status(L"Ready — authenticated matching LayerD connection.",
               StatusVoice::silent);
    automation_editor_->restore_live_subscription();
    if (!document_.path().empty() && !has_unsaved_changes())
      notify_profile_committed(document_.path(), document_.profile());
  }
  if (step.session_acknowledged)
    set_status(L"Editing bars on the selected window.");
  for (const auto &anchor : step.anchors) {
    panel_preview_.pending_anchor =
        PanelPreview::PendingAnchor{anchor.panel_id, anchor.x, anchor.y};
    select_bar_by_id(anchor.panel_id);
    InvalidateRect(panel_preview_.window, nullptr, TRUE);
    // An incoming final applies once locally and is never echoed as final.
    if (anchor.final) (void)commit_pending_anchor(false);
  }
  if (step.ended) {
    switch (step.ended->reason) {
    case LiveEditEndReason::daemon_error:
      end_edit_on_window(
          L"LayerD ended editing: " + to_wide(step.ended->text), false);
      break;
    case LiveEditEndReason::daemon_ended:
      end_edit_on_window(
          step.ended->text.empty()
              ? std::wstring(L"LayerD ended the on-window edit session.")
              : L"LayerD ended editing: " + to_wide(step.ended->text),
          false);
      break;
    case LiveEditEndReason::connection_lost:
      end_edit_on_window(
          L"Live editor connection lost; normal overlays restored.", false);
      break;
    case LiveEditEndReason::target_closed:
      end_edit_on_window(L"Edit target closed or minimized.", false);
      break;
    }
  }
  if (step.begin_ready) begin_edit_on_window(*step.begin_ready);
  if (step.pending_expired) {
    set_status(L"Live editing is unavailable — restart LayerD to update its protocol version.");
    themed_message_box(shell_.window,
                L"LayerD did not expose the live-edit service. Exit the "
                L"older LayerD from its tray icon, then try Position on "
                L"Window again.",
                L"Restart LayerD", MB_OK | MB_ICONINFORMATION);
  }
  if (step.live_profile_due) flush_live_profile();
  if (step.timer_interval)
    SetTimer(shell_.window, live_channel_timer, *step.timer_interval, nullptr);

  refresh_connection_status();
  refresh_title();
}

void EditorWindow::handle_live_message(
    const layerc::core::LiveMessage &message) {
  if (message.type == layerc::core::LiveMessageType::runtime_status) {
    shell_.visual_runtime_status = to_wide(message.text);
    visual_editor_.refresh();
    return;
  }
  if (message.type == layerc::core::LiveMessageType::diagnostics_reply) {
    pack_dialogs_.runtime_diagnostics_json = message.text;
    refresh_runtime_diagnostics_dialog();
    return;
  }
  if (automation_editor_->handle_live_message(message)) return;
  if (message.type == layerc::core::LiveMessageType::error)
    set_status(L"LayerD: " + to_wide(message.text));
}

void EditorWindow::send_live_profile() { live_edit_.request_live_profile(); }

void EditorWindow::flush_live_profile() {
  if (!live_edit_.editing() || !draft_controls_valid()) return;
  if (layerc::core::validate_profile(document_.profile())) return;
  (void)live_edit_.send_live_profile(
      layerc::core::serialize_profile_json(document_.profile()));
}

void EditorWindow::send_live_anchor(const bool final) {
  const auto *panel = selected_bar();
  if (panel == nullptr) return;
  const auto effective = preview_panel(*panel);
  (void)live_edit_.send_anchor(effective.id, effective.anchor_x,
                               effective.anchor_y, final);
}

void EditorWindow::select_bar_by_id(const std::string_view panel_id) {
  const auto &panels = document_.profile().panels;
  const auto found = std::ranges::find(panels, panel_id, &layerc::core::Panel::id);
  if (found != panels.end())
    refresh_bar_list(static_cast<std::size_t>(found - panels.begin()));
}

void EditorWindow::edit_on_window() {
  if (live_edit_.session_id() != 0) {
    end_edit_on_window(L"On-window editing ended.");
    return;
  }
  if (!require_stored_profile(
          L"Save this new profile once before editing it on a window."))
    return;

  auto matching_profile = document_.profile();
  matching_profile.enabled = true;
  std::vector<HWND> matches;
  for (const auto &candidate : layerc::platform::enumerate_attachable_windows()) {
    if (candidate.process_id == GetCurrentProcessId()) continue;
    if (matching_profile.game.global ||
        layerc::platform::profile_matches_window(candidate, matching_profile))
      matches.push_back(candidate.window);
  }
  const HWND selected = selected_task_window();
  if (selected != nullptr && IsWindow(selected) &&
      std::ranges::find(matches, selected) != matches.end()) {
    begin_edit_on_window(selected);
  } else if (matches.size() == 1) {
    begin_edit_on_window(matches.front());
  } else {
    task_picker_.request_live_edit();
    show_tasks();
    set_status(matches.empty()
                   ? L"No automatic match — select an eligible window in Detected tasks."
                   : L"Multiple windows match — select one in Detected tasks.");
  }
}

void EditorWindow::edit_selected_task() {
  const auto *candidate = require_selected_task();
  if (candidate == nullptr) return;
  auto matching = document_.profile();
  matching.enabled = true;
  if (!matching.game.global &&
      !layerc::platform::profile_matches_window(*candidate, matching)) {
    themed_message_box(task_picker_.window,
                L"That window does not match this profile. Use selected first if you want to change the profile matcher.",
                L"Window does not match", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (task_picker_.awaiting_live_edit())
    task_picker_.clear_request();
  begin_edit_on_window(candidate->window);
}

void EditorWindow::begin_edit_on_window(const HWND target) {
  if (target == nullptr || !IsWindow(target)) return;
  DWORD target_process_id{};
  GetWindowThreadProcessId(target, &target_process_id);
  if (target_process_id == 0 || target_process_id == GetCurrentProcessId()) {
    set_status(
        L"LayerC Editor cannot attach edit overlays to its own windows.");
    return;
  }
  if (!confirm_v2_upgrade()) {
    set_status(L"On-window editing needs the profile format upgrade, which was not approved.");
    return;
  }
  const auto *panel = selected_bar();
  const auto begun = live_edit_.request_begin(
      target,
      {.profile_key = layerc::platform::canonical_profile_key(document_.path()),
       .panel_id = panel == nullptr ? std::string{} : panel->id,
       .profile_json = layerc::core::serialize_profile_json(document_.profile())},
      live_edit_await_ms);
  if (begun == LiveEditBegin::awaiting_daemon) {
    (void)start_daemon();
    set_status(L"Waiting for LayerD's live-edit service...",
               StatusVoice::silent);
    return;
  }
  set_owner_draw_checkbox(GetDlgItem(shell_.window, IDC_DIRECT_DRAG), false);
  EnableWindow(GetDlgItem(shell_.window, IDC_DIRECT_DRAG), TRUE);
  set_text(GetDlgItem(shell_.window, IDC_POSITION_ON_WINDOW), L"&End positioning");
  refresh_bar_controls();
  set_status(L"Starting mirrored on-window editing; game overlay stays click-through.");
}

void EditorWindow::end_edit_on_window(const std::wstring_view status,
                                      const bool notify_daemon) {
  // A cancelled gesture is discarded, never committed.
  panel_preview_.finish_drag();
  if (GetCapture() == panel_preview_.window) ReleaseCapture();
  (void)live_edit_.end(notify_daemon);
  if (task_picker_.awaiting_live_edit())
    task_picker_.clear_request();
  set_owner_draw_checkbox(GetDlgItem(shell_.window, IDC_DIRECT_DRAG), false);
  EnableWindow(GetDlgItem(shell_.window, IDC_DIRECT_DRAG), FALSE);
  set_text(GetDlgItem(shell_.window, IDC_POSITION_ON_WINDOW),
           L"&Position on window");
  refresh_bar_controls();
  if (!status.empty()) set_status(status);
}

void EditorWindow::set_direct_drag_enabled(const bool enabled) {
  if (!live_edit_.editing()) return;
  if (enabled && live_edit_.direct_drag_notice_needed() &&
      themed_message_box(shell_.window,
                  L"Direct drag temporarily lets LayerD receive clicks only inside visible bar rectangles. It uses no injection or game hooks, but anti-cheat compatibility cannot be guaranteed. Continue?",
                  L"Enable direct drag", MB_OKCANCEL | MB_ICONINFORMATION) !=
          IDOK) {
    set_owner_draw_checkbox(GetDlgItem(shell_.window, IDC_DIRECT_DRAG), false);
    return;
  }
  if (enabled) live_edit_.note_direct_drag_notice();
  set_owner_draw_checkbox(GetDlgItem(shell_.window, IDC_DIRECT_DRAG), enabled);
  (void)live_edit_.send_direct_drag(enabled);
  set_status(enabled ? L"Direct drag enabled for this edit session."
                     : L"Mirrored click-through editing restored.");
}

void EditorWindow::undo() {
  if (!document_.undo()) return;
  refresh_profile_controls();
  sync_current_profile_entry();
  schedule_autosave(true);
  if (live_publish_allowed(document_)) send_live_profile();
  refresh_title();
}

void EditorWindow::redo() {
  if (!document_.redo()) return;
  refresh_profile_controls();
  sync_current_profile_entry();
  schedule_autosave(true);
  if (live_publish_allowed(document_)) send_live_profile();
  refresh_title();
}

} // namespace layerc::editor
