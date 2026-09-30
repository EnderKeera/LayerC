#include "editor_window.hpp"
#include "automation_editor.hpp"
#include "editor_resource.h"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
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

void EditorWindow::reset_history() {
  document_.history().reset(document_.profile(), document_.visual(), document_.automation());
}

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

void EditorWindow::record_profile_change(const ChangeKind kind) {
  if (shell_.syncing) return;
  if (document_.history().is_baseline(document_.profile(), document_.visual(), document_.automation())) {
    if (!draft_controls_valid()) {
      document_.set_dirty(true);
      schedule_autosave(false);
    }
    return;
  }
  (void)document_.history().record(document_.profile(), document_.visual(), document_.automation(),
                        kind == ChangeKind::text);
  document_.set_dirty(true);
  schedule_autosave(kind != ChangeKind::text);
  if (live_publish_allowed(document_)) send_live_profile();
}

void EditorWindow::schedule_autosave(const bool immediate) {
  if (document_.path().empty()) {
    set_status(L"Draft changed — use Save once to create the profile.");
    return;
  }
  KillTimer(shell_.window, autosave_timer);
  SetTimer(shell_.window, autosave_timer, immediate ? 1U : 400U, nullptr);
  if (!immediate) set_status(L"Editing — autosave pending.");
}

bool EditorWindow::draft_controls_valid() const {
  auto &self = const_cast<EditorWindow &>(*this);
  auto *panel = self.panel_preview_.controls_panel < self.document_.profile().panels.size()
                    ? &self.document_.profile().panels[self.panel_preview_.controls_panel]
                    : nullptr;
  const ProfileFieldContext context{
      self.shell_.window, self.document_.profile(), self.selected_cooldown(), panel};
  return profile_fields_valid(context);
}

bool EditorWindow::require_stored_profile(const wchar_t *const first_save_message) {
  if (document_.path().empty()) {
    themed_message_box(shell_.window, first_save_message, L"Create profile first",
                MB_OK | MB_ICONINFORMATION);
    return false;
  }
  return !document_.dirty() || flush_autosave();
}

bool EditorWindow::flush_autosave(const bool allow_upgrade_prompt) {
  KillTimer(shell_.window, autosave_timer);
  document_.history().end_text_coalescing();
  if (!document_.dirty()) return true;
  if (document_.path().empty() || !draft_controls_valid()) {
    set_status(document_.path().empty()
                   ? L"Draft is not created yet — use Save."
                   : L"Not autosaved — finish the invalid field.");
    return false;
  }
  switch (commit_bundle(document_.path(),
                        {.prompt_upgrade = allow_upgrade_prompt})) {
  case CommitResult::ok:
    mark_committed();
    set_status(L"Autosaved — LayerD updated live.");
    return true;
  case CommitResult::invalid_fields:
    set_status(L"Not autosaved — finish the invalid field.");
    break;
  case CommitResult::invalid_profile:
    set_status(L"Not autosaved — profile validation failed.");
    break;
  case CommitResult::upgrade_declined:
    set_status(allow_upgrade_prompt
                   ? L"Not saved — the profile format upgrade was not confirmed."
                   : L"Autosave paused — use Save to approve the profile format upgrade.");
    break;
  case CommitResult::blocked_by_sidecar:
    set_status(L"Not autosaved — repair the invalid visual or automation settings first.");
    break;
  case CommitResult::write_failed:
    set_status(write_failure_message());
    break;
  }
  return false;
}

void EditorWindow::notify_profile_committed(
    const std::filesystem::path &path,
    const layerc::core::GameProfile &profile) {
  live_edit_.advance_revision();
  if (live_edit_.channel().connected() && live_edit_.package_authenticated()) {
    (void)live_edit_.channel().send({
        .type = layerc::core::LiveMessageType::committed_profile,
        .revision = live_edit_.revision(),
        .profile_key = layerc::platform::canonical_profile_key(path),
        .profile_json = layerc::core::serialize_profile_json(profile),
    });
  } else {
    (void)start_daemon();
    (void)layerc::platform::signal_profile_reload();
  }
}

void EditorWindow::poll_live_channel() {
  for (auto &result : live_edit_.channel().poll()) {
    if (const auto *error =
            std::get_if<layerc::core::LiveProtocolError>(&result)) {
      set_status(L"Live channel error: " + to_wide(error->message));
      continue;
    }
    handle_live_message(std::get<layerc::core::LiveMessage>(result));
  }
  if (live_edit_.channel().take_connection_changed()) {
    live_edit_.set_package_authenticated(false);
    if (live_edit_.channel().connected()) {
      (void)live_edit_.channel().send({
          .type = layerc::core::LiveMessageType::handshake,
          .revision = live_edit_.revision(),
          .text = layerc::core::package_identity("LayerE"),
      });
      set_status(L"Authenticating LayerD package...");
    } else if (live_edit_.session_id() != 0) {
      end_edit_on_window(
          L"Live editor connection lost; normal overlays restored.", false);
    }
  }

  if (live_edit_.pending_target() != nullptr) {
    if (live_edit_.channel().connected() && live_edit_.package_authenticated()) {
      begin_edit_on_window(live_edit_.take_pending_target());
    } else if (GetTickCount64() >= live_edit_.pending_deadline()) {
      live_edit_.clear_pending_target();
      set_status(L"Live editing is unavailable — restart LayerD to update its protocol version.");
      themed_message_box(shell_.window,
                  L"LayerD did not expose the live-edit service. Exit the "
                  L"older LayerD from its tray icon, then try Position on "
                  L"Window again.",
                  L"Restart LayerD", MB_OK | MB_ICONINFORMATION);
    }
  }

  refresh_connection_status();
  refresh_title();

  const ULONGLONG now = GetTickCount64();
  if (live_edit_.package_authenticated() && live_edit_.session_id() != 0 &&
      now - live_edit_.last_heartbeat_send() >= 250) {
    live_edit_.note_heartbeat_send(now);
    (void)live_edit_.channel().send({
        .type = layerc::core::LiveMessageType::heartbeat,
        .revision = live_edit_.revision(),
        .edit_session = live_edit_.session_id(),
    });
    if (live_edit_.target() == nullptr || !IsWindow(live_edit_.target()) ||
        IsIconic(live_edit_.target()))
      end_edit_on_window(L"Edit target closed or minimized.");
  }
  // Poll at frame rate only while an edit session exchanges drags and
  // heartbeats; an idle connection needs no more than the reconnect cadence.
  const UINT desired_interval =
      live_edit_.channel().connected() && live_edit_.session_id() != 0 ? 16U
                                                                         : 100U;
  if (desired_interval != live_edit_.timer_interval()) {
    live_edit_.set_timer_interval(desired_interval);
    SetTimer(shell_.window, live_channel_timer, live_edit_.timer_interval(), nullptr);
  }
}

void EditorWindow::handle_live_message(
    const layerc::core::LiveMessage &message) {
  if (message.type == layerc::core::LiveMessageType::runtime_status) {
    shell_.visual_runtime_status = to_wide(message.text);
    refresh_visual_controls();
    return;
  }
  if (message.type == layerc::core::LiveMessageType::diagnostics_reply) {
    pack_dialogs_.runtime_diagnostics_json = message.text;
    refresh_runtime_diagnostics_dialog();
    return;
  }
  if (automation_editor_->handle_live_message(message)) return;
  if (message.type == layerc::core::LiveMessageType::error) {
    if (live_edit_.session_id() != 0 &&
        message.edit_session == live_edit_.session_id()) {
      end_edit_on_window(L"LayerD ended editing: " + to_wide(message.text),
                         false);
    } else {
      set_status(L"LayerD: " + to_wide(message.text));
    }
    return;
  }
  if (message.type == layerc::core::LiveMessageType::end_edit) {
    if (live_edit_.session_id() != 0 &&
        message.edit_session == live_edit_.session_id()) {
      const std::wstring status =
          message.text.empty()
              ? L"LayerD ended the on-window edit session."
              : L"LayerD ended editing: " + to_wide(message.text);
      end_edit_on_window(status, false);
    }
    return;
  }
  if (message.type == layerc::core::LiveMessageType::acknowledgement) {
    if (!live_edit_.package_authenticated()) {
      const auto expected = layerc::core::package_identity("LayerD") + ";";
      if (!message.text.starts_with(expected)) {
        set_status(L"Connected LayerD is not from the current LayerC package.");
        return;
      }
      live_edit_.set_package_authenticated(true);
      set_status(L"Ready — authenticated matching LayerD connection.");
      automation_editor_->restore_live_subscription();

      if (!document_.path().empty() && !document_.dirty())
        notify_profile_committed(document_.path(), document_.profile());
      return;
    }
    if (message.edit_session == live_edit_.session_id() && live_edit_.session_id() != 0)
      set_status(L"Editing bars on the selected window.");
    return;
  }
  if (message.type != layerc::core::LiveMessageType::anchor_changed ||
      message.edit_session != live_edit_.session_id())
    return;
  if (layerc::core::revision_decision(message.revision, live_edit_.peer_revision()) !=
      layerc::core::RevisionDecision::accept)
    return;
  live_edit_.observe_peer(message.revision);
  const auto panel = std::ranges::find(document_.profile().panels,
                                       message.panel_id,
                                       &layerc::core::Panel::id);
  if (panel == document_.profile().panels.end()) return;
  panel->anchor_x = message.anchor_x;
  panel->anchor_y = message.anchor_y;
  select_bar_by_id(message.panel_id);
  InvalidateRect(panel_preview_.window, nullptr, TRUE);
  if (message.final)
    record_profile_change(ChangeKind::drag);
}

void EditorWindow::send_live_profile() {
  if (live_edit_.session_id() == 0 || live_profile_pending_) return;
  live_profile_pending_ = true;
  SetTimer(shell_.window, live_profile_timer, live_profile_interval_ms,
           nullptr);
}

void EditorWindow::flush_live_profile() {
  KillTimer(shell_.window, live_profile_timer);
  live_profile_pending_ = false;
  if (live_edit_.session_id() == 0 || !draft_controls_valid()) return;
  const auto serialized = layerc::core::serialize_profile_json(document_.profile());
  if (!std::holds_alternative<layerc::core::GameProfile>(
          layerc::core::parse_profile_json(serialized)))
    return;
  live_edit_.advance_revision();
  (void)live_edit_.channel().send({
      .type = layerc::core::LiveMessageType::live_profile,
      .revision = live_edit_.revision(),
      .edit_session = live_edit_.session_id(),
      .profile_json = serialized,
  });
}

void EditorWindow::send_live_anchor(const bool final) {
  const auto *panel = selected_bar();
  if (live_edit_.session_id() == 0 || panel == nullptr) return;
  const ULONGLONG now = GetTickCount64();
  if (!final && now - live_edit_.last_drag_send() < 16) return;
  live_edit_.note_drag_send(now);
  live_edit_.advance_revision();
  (void)live_edit_.channel().send({
      .type = layerc::core::LiveMessageType::anchor_changed,
      .revision = live_edit_.revision(),
      .edit_session = live_edit_.session_id(),
      .panel_id = panel->id,
      .anchor_x = panel->anchor_x,
      .anchor_y = panel->anchor_y,
      .final = final,
  });
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
                   ? L"No automatic match — select an eligible window in Detected Tasks."
                   : L"Multiple windows match — select one in Detected Tasks.");
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
                L"That window does not match this profile. Use Selected Task first if you want to change the profile matcher.",
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
  if (!live_edit_.channel().connected() || !live_edit_.package_authenticated()) {
    (void)start_daemon();
    live_edit_.await_target(target, GetTickCount64() + 2000);
    set_status(L"Waiting for LayerD's live-edit service...");
    return;
  }
  const auto session_id =
      (static_cast<std::uint64_t>(GetTickCount64()) << 16U) ^
      (static_cast<std::uint64_t>(GetCurrentProcessId()) << 1U) ^
      reinterpret_cast<std::uintptr_t>(target);
  live_edit_.begin_session(target, session_id);
  set_owner_draw_checkbox(GetDlgItem(shell_.window, IDC_DIRECT_DRAG), false);
  EnableWindow(GetDlgItem(shell_.window, IDC_DIRECT_DRAG), TRUE);
  set_text(GetDlgItem(shell_.window, IDC_POSITION_ON_WINDOW), L"&End positioning");
  refresh_bar_controls();
  const auto *panel = selected_bar();
  live_edit_.advance_revision();
  (void)live_edit_.channel().send({
      .type = layerc::core::LiveMessageType::begin_edit,
      .revision = live_edit_.revision(),
      .edit_session = live_edit_.session_id(),
      .target_window = reinterpret_cast<std::uintptr_t>(target),
      .profile_key = layerc::platform::canonical_profile_key(document_.path()),
      .panel_id = panel == nullptr ? std::string{} : panel->id,
      .profile_json = layerc::core::serialize_profile_json(document_.profile()),
  });
  live_edit_.note_heartbeat_send(GetTickCount64());
  set_status(L"Starting mirrored on-window editing; game overlay stays click-through.");
}

void EditorWindow::end_edit_on_window(const std::wstring_view status,
                                      const bool notify_daemon) {
  if (notify_daemon && live_edit_.session_id() != 0) {
    (void)live_edit_.channel().send({
        .type = layerc::core::LiveMessageType::end_edit,
        .revision = live_edit_.revision(),
        .edit_session = live_edit_.session_id(),
    });
  }
  live_edit_.clear_session();
  KillTimer(shell_.window, live_profile_timer);
  live_profile_pending_ = false;
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
  if (live_edit_.session_id() == 0) return;
  if (enabled && !live_edit_.direct_drag_notice_shown() &&
      themed_message_box(shell_.window,
                  L"Direct Drag temporarily lets LayerD receive clicks only inside visible bar rectangles. It uses no injection or game hooks, but anti-cheat compatibility cannot be guaranteed. Continue?",
                  L"Enable Direct Drag", MB_OKCANCEL | MB_ICONINFORMATION) !=
          IDOK) {
    set_owner_draw_checkbox(GetDlgItem(shell_.window, IDC_DIRECT_DRAG), false);
    return;
  }
  if (enabled) live_edit_.note_direct_drag_notice();
  set_owner_draw_checkbox(GetDlgItem(shell_.window, IDC_DIRECT_DRAG), enabled);
  live_edit_.advance_revision();
  (void)live_edit_.channel().send({
      .type = layerc::core::LiveMessageType::set_direct_drag,
      .revision = live_edit_.revision(),
      .edit_session = live_edit_.session_id(),
      .direct_drag = enabled,
  });
  set_status(enabled ? L"Direct Drag enabled for this edit session."
                     : L"Mirrored click-through editing restored.");
}

void EditorWindow::undo() {
  auto previous = document_.history().undo(document_.profile(), document_.visual(), document_.automation());
  if (!previous.has_value()) return;
  apply_history_snapshot(std::move(*previous));
}

void EditorWindow::redo() {
  auto next = document_.history().redo(document_.profile(), document_.visual(), document_.automation());
  if (!next.has_value()) return;
  apply_history_snapshot(std::move(*next));
}

void EditorWindow::apply_history_snapshot(EditorProfileSnapshot snapshot) {
  document_.profile() = std::move(snapshot.profile);
  document_.visual() = std::move(snapshot.visual_profile);
  document_.automation() = std::move(snapshot.automation_profile);
  document_.set_dirty(document_.history().is_dirty(
      document_.profile(), document_.visual(), document_.automation(),
      !document_.path().empty()));
  refresh_profile_controls();
  sync_current_profile_entry();
  schedule_autosave(true);
  if (live_publish_allowed(document_)) send_live_profile();
}

} // namespace layerc::editor
