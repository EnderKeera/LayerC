#include "editor_window.hpp"
#include "automation_editor.hpp"
#include "editor_resource.h"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <string>
#include <variant>

#include <commctrl.h>
#include <windows.h>

namespace layerc::editor {
namespace {

bool bounded_decimal(const std::wstring &text, const std::uint32_t minimum,
                     const std::uint32_t maximum) {
  if (text.empty()) return false;
  wchar_t *parsed_end{};
  const unsigned long parsed = wcstoul(text.c_str(), &parsed_end, 10);
  return parsed_end != text.c_str() && parsed_end != nullptr &&
         *parsed_end == L'\0' && parsed >= minimum && parsed <= maximum;
}

} // namespace

void EditorWindow::reset_history() {
  history_.reset(profile_, visual_profile_, automation_profile_);
}

void EditorWindow::sync_current_profile_entry() {
  if (current_path_.empty() || profile_list_ == nullptr) return;
  const auto current_key =
      layerc::platform::canonical_profile_key(current_path_);
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    auto &entry = entries_[index];
    if (layerc::platform::canonical_profile_key(entry.path) != current_key)
      continue;
    entry.enabled = profile_.enabled;
    entry.label = profile_list_label(profile_);
    const bool was_syncing = syncing_;
    syncing_ = true;
    ListView_SetItemText(profile_list_, static_cast<int>(index), 0,
                         entry.label.data());
    ListView_SetCheckState(profile_list_, static_cast<int>(index),
                           entry.enabled);
    syncing_ = was_syncing;
    InvalidateRect(profile_list_, nullptr, TRUE);
    return;
  }
}

void EditorWindow::record_profile_change(const ChangeKind kind) {
  if (syncing_) return;
  if (history_.is_baseline(profile_, visual_profile_, automation_profile_)) {
    if (!draft_controls_valid()) {
      dirty_ = true;
      schedule_autosave(false);
    }
    return;
  }
  (void)history_.record(profile_, visual_profile_, automation_profile_,
                        kind == ChangeKind::text);
  dirty_ = true;
  schedule_autosave(kind != ChangeKind::text);
  // Do not publish an in-memory schema-v3 migration before the user has
  // approved making that one-way disk upgrade.
  if ((!legacy_base_schema_ && !legacy_visual_schema_) ||
      v2_upgrade_acknowledged_)
    send_live_profile();
}

void EditorWindow::schedule_autosave(const bool immediate) {
  if (current_path_.empty()) {
    set_status(L"Draft changed — use Save once to create the profile.");
    return;
  }
  KillTimer(dialog_, autosave_timer);
  SetTimer(dialog_, autosave_timer, immediate ? 1U : 400U, nullptr);
  if (!immediate) set_status(L"Editing — autosave pending.");
}

bool EditorWindow::draft_controls_valid() const {
  if (control_text(GetDlgItem(dialog_, IDC_PROFILE_NAME)).empty() ||
      control_text(GetDlgItem(dialog_, IDC_PROFILE_ID)).empty() ||
      control_text(GetDlgItem(dialog_, IDC_COOLDOWN_NAME)).empty() ||
      !parse_duration_ms(
          control_text(GetDlgItem(dialog_, IDC_COOLDOWN_DURATION))) ||
      !bounded_decimal(
          control_text(GetDlgItem(dialog_, IDC_COOLDOWN_CHARGES)), 1, 99))
    return false;
  if (bar_controls_panel_ < profile_.panels.size()) {
    if (!bounded_decimal(control_text(GetDlgItem(dialog_, IDC_BAR_WIDTH)), 40,
                         1200) ||
        !bounded_decimal(control_text(GetDlgItem(dialog_, IDC_BAR_HEIGHT)), 16,
                         240))
      return false;
  }
  return true;
}

bool EditorWindow::flush_autosave(const bool allow_upgrade_prompt) {
  KillTimer(dialog_, autosave_timer);
  history_.end_text_coalescing();
  if (!dirty_) return true;
  if (current_path_.empty() || !draft_controls_valid()) {
    set_status(current_path_.empty()
                   ? L"Draft is not created yet — use Save."
                   : L"Not autosaved — finish the invalid field.");
    return false;
  }
  if (!visual_profile_ &&
      visual_sidecar_state_ ==
          layerc::platform::VisualSidecarState::invalid) {
    set_status(L"Not autosaved — repair the invalid visual sidecar first.");
    return false;
  }
  if (!automation_profile_ &&
      automation_sidecar_state_ ==
          layerc::platform::AutomationSidecarState::invalid) {
    set_status(L"Not autosaved — repair the invalid automation sidecar first.");
    return false;
  }
  sync_profile_from_controls();
  const auto serialized = layerc::core::serialize_profile_json(profile_);
  const auto parsed = layerc::core::parse_profile_json(serialized);
  const auto *validated = std::get_if<layerc::core::GameProfile>(&parsed);
  if (validated == nullptr) {
    set_status(L"Not autosaved — profile validation failed.");
    return false;
  }
  profile_ = *validated;
  if (visual_profile_) visual_profile_->profile_id = profile_.id;
  if (automation_profile_) automation_profile_->profile_id = profile_.id;
  if ((legacy_base_schema_ || legacy_visual_schema_) &&
      !v2_upgrade_acknowledged_) {
    if (!allow_upgrade_prompt) {
      set_status(
          L"Autosave paused — use Save to approve the schema-v3 upgrade.");
      return false;
    }
    if (!confirm_v2_upgrade()) {
      set_status(L"Not saved — schema upgrade was not confirmed.");
      return false;
    }
  }
  if (!layerc::platform::write_profile_bundle(
          current_path_, profile_, visual_profile_, automation_profile_)) {
    set_status(L"Autosave failed — the last consistent profile bundle remains active.");
    return false;
  }
  history_.rebase(profile_, visual_profile_, automation_profile_);
  dirty_ = false;
  legacy_base_schema_ = false;
  legacy_visual_schema_ = false;
  v2_upgrade_acknowledged_ = false;
  sync_current_profile_entry();
  notify_profile_committed(current_path_, profile_);
  set_status(L"Autosaved — LayerD updated live.");
  return true;
}

void EditorWindow::notify_profile_committed(
    const std::filesystem::path &path,
    const layerc::core::GameProfile &profile) {
  ++revision_;
  if (live_channel_.connected() && live_package_authenticated_) {
    (void)live_channel_.send({
        .type = layerc::core::LiveMessageType::committed_profile,
        .revision = revision_,
        .profile_key =
            to_utf8(layerc::platform::canonical_profile_key(path)),
        .profile_json = layerc::core::serialize_profile_json(profile),
    });
  } else {
    (void)start_daemon();
    (void)layerc::platform::signal_profile_reload();
  }
}

void EditorWindow::poll_live_channel() {
  for (auto &result : live_channel_.poll()) {
    if (const auto *error =
            std::get_if<layerc::core::LiveProtocolError>(&result)) {
      set_status(L"Live channel error: " + to_wide(error->message));
      continue;
    }
    handle_live_message(std::get<layerc::core::LiveMessage>(result));
  }
  if (live_channel_.take_connection_changed()) {
    live_package_authenticated_ = false;
    if (live_channel_.connected()) {
      (void)live_channel_.send({
          .type = layerc::core::LiveMessageType::handshake,
          .revision = revision_,
          .text = layerc::core::package_identity("LayerE"),
      });
      set_status(L"Authenticating LayerD package...");
    } else if (edit_session_id_ != 0) {
      end_edit_on_window(
          L"Live editor connection lost; normal overlays restored.", false);
    }
  }

  if (pending_edit_target_ != nullptr) {
    if (live_channel_.connected() && live_package_authenticated_) {
      const HWND target = pending_edit_target_;
      pending_edit_target_ = nullptr;
      pending_edit_deadline_ = 0;
      begin_edit_on_window(target);
    } else if (GetTickCount64() >= pending_edit_deadline_) {
      pending_edit_target_ = nullptr;
      pending_edit_deadline_ = 0;
      set_status(L"Live editing is unavailable — restart LayerD to update its protocol version.");
      MessageBoxW(dialog_,
                  L"LayerD did not expose the live-edit service. Exit the "
                  L"older LayerD from its tray icon, then try Position on "
                  L"Window again.",
                  L"Restart LayerD", MB_OK | MB_ICONINFORMATION);
    }
  }

  const ULONGLONG now = GetTickCount64();
  if (live_package_authenticated_ && edit_session_id_ != 0 &&
      now - last_heartbeat_send_ >= 250) {
    last_heartbeat_send_ = now;
    (void)live_channel_.send({
        .type = layerc::core::LiveMessageType::heartbeat,
        .revision = revision_,
        .edit_session = edit_session_id_,
    });
    if (edit_target_ == nullptr || !IsWindow(edit_target_) ||
        IsIconic(edit_target_))
      end_edit_on_window(L"Edit target closed or minimized.");
  }
  const UINT desired_interval = live_channel_.connected() ? 16U : 100U;
  if (desired_interval != live_timer_interval_) {
    live_timer_interval_ = desired_interval;
    SetTimer(dialog_, live_channel_timer, live_timer_interval_, nullptr);
  }
}

void EditorWindow::handle_live_message(
    const layerc::core::LiveMessage &message) {
  if (message.type == layerc::core::LiveMessageType::runtime_status) {
    visual_runtime_status_ = to_wide(message.text);
    refresh_visual_controls();
    return;
  }
  if (message.type == layerc::core::LiveMessageType::diagnostics_reply) {
    runtime_diagnostics_json_ = message.text;
    refresh_runtime_diagnostics_dialog();
    return;
  }
  if (automation_editor_->handle_live_message(message)) return;
  if (message.type == layerc::core::LiveMessageType::error) {
    if (edit_session_id_ != 0 &&
        message.edit_session == edit_session_id_) {
      end_edit_on_window(L"LayerD ended editing: " + to_wide(message.text),
                         false);
    } else {
      set_status(L"LayerD: " + to_wide(message.text));
    }
    return;
  }
  if (message.type == layerc::core::LiveMessageType::end_edit) {
    if (edit_session_id_ != 0 &&
        message.edit_session == edit_session_id_) {
      const std::wstring status =
          message.text.empty()
              ? L"LayerD ended the on-window edit session."
              : L"LayerD ended editing: " + to_wide(message.text);
      end_edit_on_window(status, false);
    }
    return;
  }
  if (message.type == layerc::core::LiveMessageType::acknowledgement) {
    if (!live_package_authenticated_) {
      const auto expected = layerc::core::package_identity("LayerD") + ";";
      if (!message.text.starts_with(expected)) {
        set_status(L"Connected LayerD is not from the current LayerC package.");
        return;
      }
      live_package_authenticated_ = true;
      set_status(L"Ready — authenticated matching LayerD connection.");
      automation_editor_->restore_live_subscription();

      if (!current_path_.empty() && !dirty_)
        notify_profile_committed(current_path_, profile_);
      return;
    }
    if (message.edit_session == edit_session_id_ && edit_session_id_ != 0)
      set_status(L"Editing bars on the selected window.");
    return;
  }
  if (message.type != layerc::core::LiveMessageType::anchor_changed ||
      message.edit_session != edit_session_id_)
    return;
  if (layerc::core::revision_decision(message.revision, peer_revision_) !=
      layerc::core::RevisionDecision::accept)
    return;
  peer_revision_ = message.revision;
  const auto panel = std::ranges::find_if(
      profile_.panels, [&](const layerc::core::Panel &candidate) {
        return candidate.id == message.panel_id;
      });
  if (panel == profile_.panels.end()) return;
  panel->anchor_x = message.anchor_x;
  panel->anchor_y = message.anchor_y;
  select_bar_by_id(message.panel_id);
  InvalidateRect(preview_, nullptr, TRUE);
  if (message.final)
    record_profile_change(ChangeKind::drag);
}

void EditorWindow::send_live_profile() {
  if (edit_session_id_ == 0 || !draft_controls_valid()) return;
  const auto serialized = layerc::core::serialize_profile_json(profile_);
  if (!std::holds_alternative<layerc::core::GameProfile>(
          layerc::core::parse_profile_json(serialized)))
    return;
  ++revision_;
  (void)live_channel_.send({
      .type = layerc::core::LiveMessageType::live_profile,
      .revision = revision_,
      .edit_session = edit_session_id_,
      .profile_json = serialized,
  });
}

void EditorWindow::send_live_anchor(const bool final) {
  const auto *panel = selected_bar();
  if (edit_session_id_ == 0 || panel == nullptr) return;
  const ULONGLONG now = GetTickCount64();
  if (!final && now - last_live_drag_send_ < 16) return;
  last_live_drag_send_ = now;
  ++revision_;
  (void)live_channel_.send({
      .type = layerc::core::LiveMessageType::anchor_changed,
      .revision = revision_,
      .edit_session = edit_session_id_,
      .panel_id = panel->id,
      .anchor_x = panel->anchor_x,
      .anchor_y = panel->anchor_y,
      .final = final,
  });
}

void EditorWindow::select_bar_by_id(const std::string_view panel_id) {
  for (std::size_t index = 0; index < profile_.panels.size(); ++index) {
    if (profile_.panels[index].id == panel_id) {
      refresh_bar_list(index);
      return;
    }
  }
}

void EditorWindow::edit_on_window() {
  if (edit_session_id_ != 0) {
    end_edit_on_window(L"On-window editing ended.");
    return;
  }
  if (current_path_.empty()) {
    MessageBoxW(dialog_, L"Save this new profile once before editing it on a window.",
                L"Create profile first", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (dirty_ && !flush_autosave()) return;

  auto matching_profile = profile_;
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
    awaiting_edit_target_ = true;
    show_tasks();
    set_status(matches.empty()
                   ? L"No automatic match — select an eligible window in Detected Tasks."
                   : L"Multiple windows match — select one in Detected Tasks.");
  }
}

void EditorWindow::edit_selected_task() {
  const auto *candidate = require_selected_task();
  if (candidate == nullptr) return;
  auto matching = profile_;
  matching.enabled = true;
  if (!matching.game.global &&
      !layerc::platform::profile_matches_window(*candidate, matching)) {
    MessageBoxW(task_dialog_,
                L"That window does not match this profile. Use Selected Task first if you want to change the profile matcher.",
                L"Window does not match", MB_OK | MB_ICONINFORMATION);
    return;
  }
  awaiting_edit_target_ = false;
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
    set_status(L"On-window editing was not started because the schema-v3 upgrade was not approved.");
    return;
  }
  if (!live_channel_.connected() || !live_package_authenticated_) {
    (void)start_daemon();
    pending_edit_target_ = target;
    pending_edit_deadline_ = GetTickCount64() + 2000;
    set_status(L"Waiting for LayerD's live-edit service...");
    return;
  }
  edit_target_ = target;
  edit_session_id_ =
      (static_cast<std::uint64_t>(GetTickCount64()) << 16U) ^
      (static_cast<std::uint64_t>(GetCurrentProcessId()) << 1U) ^
      reinterpret_cast<std::uintptr_t>(target);
  if (edit_session_id_ == 0) edit_session_id_ = 1;
  peer_revision_ = 0;
  direct_drag_enabled_ = false;
  direct_drag_notice_shown_ = false;
  set_owner_draw_checkbox(GetDlgItem(dialog_, IDC_DIRECT_DRAG), false);
  EnableWindow(GetDlgItem(dialog_, IDC_DIRECT_DRAG), TRUE);
  set_text(GetDlgItem(dialog_, IDC_POSITION_ON_WINDOW), L"End Positioning");
  const auto *panel = selected_bar();
  ++revision_;
  (void)live_channel_.send({
      .type = layerc::core::LiveMessageType::begin_edit,
      .revision = revision_,
      .edit_session = edit_session_id_,
      .target_window = reinterpret_cast<std::uintptr_t>(target),
      .profile_key = to_utf8(
          layerc::platform::canonical_profile_key(current_path_)),
      .panel_id = panel == nullptr ? std::string{} : panel->id,
      .profile_json = layerc::core::serialize_profile_json(profile_),
  });
  last_heartbeat_send_ = GetTickCount64();
  set_status(L"Starting mirrored on-window editing; game overlay stays click-through.");
}

void EditorWindow::end_edit_on_window(const std::wstring_view status,
                                      const bool notify_daemon) {
  if (notify_daemon && edit_session_id_ != 0) {
    (void)live_channel_.send({
        .type = layerc::core::LiveMessageType::end_edit,
        .revision = revision_,
        .edit_session = edit_session_id_,
    });
  }
  edit_session_id_ = 0;
  peer_revision_ = 0;
  edit_target_ = nullptr;
  pending_edit_target_ = nullptr;
  pending_edit_deadline_ = 0;
  direct_drag_enabled_ = false;
  direct_drag_notice_shown_ = false;
  awaiting_edit_target_ = false;
  set_owner_draw_checkbox(GetDlgItem(dialog_, IDC_DIRECT_DRAG), false);
  EnableWindow(GetDlgItem(dialog_, IDC_DIRECT_DRAG), FALSE);
  set_text(GetDlgItem(dialog_, IDC_POSITION_ON_WINDOW),
           L"Position on Window");
  if (!status.empty()) set_status(status);
}

void EditorWindow::set_direct_drag_enabled(const bool enabled,
                                           const bool show_notice) {
  if (edit_session_id_ == 0) return;
  if (enabled && show_notice && !direct_drag_notice_shown_ &&
      MessageBoxW(dialog_,
                  L"Direct Drag temporarily lets LayerD receive clicks only inside visible bar rectangles. It uses no injection or game hooks, but anti-cheat compatibility cannot be guaranteed. Continue?",
                  L"Enable Direct Drag", MB_OKCANCEL | MB_ICONINFORMATION) !=
          IDOK) {
    set_owner_draw_checkbox(GetDlgItem(dialog_, IDC_DIRECT_DRAG), false);
    InvalidateRect(GetDlgItem(dialog_, IDC_DIRECT_DRAG), nullptr, TRUE);
    return;
  }
  if (enabled) direct_drag_notice_shown_ = true;
  direct_drag_enabled_ = enabled;
  set_owner_draw_checkbox(GetDlgItem(dialog_, IDC_DIRECT_DRAG), enabled);
  ++revision_;
  (void)live_channel_.send({
      .type = layerc::core::LiveMessageType::set_direct_drag,
      .revision = revision_,
      .edit_session = edit_session_id_,
      .direct_drag = enabled,
  });
  set_status(enabled ? L"Direct Drag enabled for this edit session."
                     : L"Mirrored click-through editing restored.");
}

void EditorWindow::undo() {
  auto previous = history_.undo(profile_, visual_profile_, automation_profile_);
  if (!previous.has_value()) return;
  apply_history_snapshot(std::move(*previous));
}

void EditorWindow::redo() {
  auto next = history_.redo(profile_, visual_profile_, automation_profile_);
  if (!next.has_value()) return;
  apply_history_snapshot(std::move(*next));
}

void EditorWindow::apply_history_snapshot(EditorProfileSnapshot snapshot) {
  profile_ = std::move(snapshot.profile);
  visual_profile_ = std::move(snapshot.visual_profile);
  automation_profile_ = std::move(snapshot.automation_profile);
  dirty_ = history_.is_dirty(profile_, visual_profile_, automation_profile_,
                             !current_path_.empty());
  refresh_profile_controls();
  sync_current_profile_entry();
  schedule_autosave(true);
  if ((!legacy_base_schema_ && !legacy_visual_schema_) ||
      v2_upgrade_acknowledged_)
    send_live_profile();
}

} // namespace layerc::editor
