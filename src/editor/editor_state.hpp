#pragma once

#include "appearance_draft.hpp"
#include "editor_util.hpp"

#include "layerc/core/live_edit.hpp"
#include "layerc/platform/live_channel.hpp"
#include "layerc/platform/native_theme.hpp"
#include "layerc/platform/window_query.hpp"

#include <commctrl.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace layerc::editor {

struct ProfileEntry {
  std::filesystem::path path;
  std::wstring label;
  bool enabled{true};
};

// Owns the main editor shell and its profile-list controls. The shell is bound
// once during WM_INITDIALOG; every borrowed child handle belongs to that window.
class EditorShell final {
public:
  [[nodiscard]] HINSTANCE &instance() noexcept { return instance_; }
  [[nodiscard]] HINSTANCE instance() const noexcept { return instance_; }
  [[nodiscard]] HWND &window() noexcept { return window_; }
  [[nodiscard]] HWND window() const noexcept { return window_; }
  [[nodiscard]] HWND &profile_list() noexcept { return profile_list_; }
  [[nodiscard]] HWND profile_list() const noexcept { return profile_list_; }
  [[nodiscard]] HWND &application_path() noexcept { return application_path_; }
  [[nodiscard]] HWND application_path() const noexcept { return application_path_; }
  [[nodiscard]] HWND &status() noexcept { return status_; }
  [[nodiscard]] HWND status() const noexcept { return status_; }
  [[nodiscard]] HFONT &font() noexcept { return font_; }
  [[nodiscard]] HFONT font() const noexcept { return font_; }
  [[nodiscard]] HIMAGELIST &profile_check_hit_images() noexcept {
    return profile_check_hit_images_;
  }
  [[nodiscard]] HIMAGELIST profile_check_hit_images() const noexcept {
    return profile_check_hit_images_;
  }
  [[nodiscard]] std::vector<ProfileEntry> &profiles() noexcept {
    return profiles_;
  }
  [[nodiscard]] const std::vector<ProfileEntry> &profiles() const noexcept {
    return profiles_;
  }
  [[nodiscard]] std::wstring &visual_runtime_status() noexcept {
    return visual_runtime_status_;
  }
  [[nodiscard]] const std::wstring &visual_runtime_status() const noexcept {
    return visual_runtime_status_;
  }
  [[nodiscard]] bool &capture_exclusion() noexcept { return capture_exclusion_; }
  [[nodiscard]] bool capture_exclusion() const noexcept {
    return capture_exclusion_;
  }
  [[nodiscard]] bool &advanced_visible() noexcept { return advanced_visible_; }
  [[nodiscard]] bool advanced_visible() const noexcept {
    return advanced_visible_;
  }
  [[nodiscard]] bool &syncing() noexcept { return syncing_; }
  [[nodiscard]] bool syncing() const noexcept { return syncing_; }

private:
  HINSTANCE instance_{};
  HWND window_{};
  HWND profile_list_{};
  HWND application_path_{};
  HWND status_{};
  HFONT font_{};
  HIMAGELIST profile_check_hit_images_{};
  std::vector<ProfileEntry> profiles_;
  std::wstring visual_runtime_status_{L"Visual recognition: connecting"};
  bool capture_exclusion_{true};
  bool advanced_visible_{};
  bool syncing_{};
};

// Owns the cooldown list and exclusive binding-capture mode. Capture is either
// idle or belongs to this editor; no second binding can be captured concurrently.
class CooldownEditor final {
public:
  [[nodiscard]] HWND &list() noexcept { return list_; }
  [[nodiscard]] HWND list() const noexcept { return list_; }
  [[nodiscard]] bool capturing_binding() const noexcept {
    return capturing_binding_;
  }
  [[nodiscard]] const std::vector<std::uint16_t> &
  pending_binding_codes() const noexcept {
    return pending_binding_codes_;
  }
  void begin_binding_capture() {
    capturing_binding_ = true;
    pending_binding_codes_.clear();
  }
  void finish_binding_capture() noexcept {
    capturing_binding_ = false;
    pending_binding_codes_.clear();
  }
  void record_binding_key(const std::uint16_t code) {
    if (std::ranges::find(pending_binding_codes_, code) ==
        pending_binding_codes_.end())
      pending_binding_codes_.push_back(code);
  }

private:
  HWND list_{};
  bool capturing_binding_{};
  std::vector<std::uint16_t> pending_binding_codes_;
};

// Owns panel-list selection, preview drag state, and the panel whose controls
// are currently projected. A completed or cancelled drag resets all drag fields.
class PanelPreview final {
public:
  [[nodiscard]] HWND &list() noexcept { return list_; }
  [[nodiscard]] HWND list() const noexcept { return list_; }
  [[nodiscard]] HWND &window() noexcept { return window_; }
  [[nodiscard]] HWND window() const noexcept { return window_; }
  [[nodiscard]] bool &dragging() noexcept { return dragging_; }
  [[nodiscard]] bool dragging() const noexcept { return dragging_; }
  [[nodiscard]] bool &drag_moved() noexcept { return drag_moved_; }
  [[nodiscard]] bool drag_moved() const noexcept { return drag_moved_; }
  [[nodiscard]] POINT &drag_start() noexcept { return drag_start_; }
  [[nodiscard]] POINT drag_start() const noexcept { return drag_start_; }
  [[nodiscard]] POINT &drag_offset() noexcept { return drag_offset_; }
  [[nodiscard]] POINT drag_offset() const noexcept { return drag_offset_; }
  [[nodiscard]] std::size_t &controls_panel() noexcept { return controls_panel_; }
  [[nodiscard]] std::size_t controls_panel() const noexcept {
    return controls_panel_;
  }

  void begin_drag(const POINT start, const POINT offset) noexcept {
    dragging_ = true;
    drag_moved_ = false;
    drag_start_ = start;
    drag_offset_ = offset;
  }
  void mark_drag_moved() noexcept { drag_moved_ = true; }
  void finish_drag() noexcept {
    dragging_ = false;
    drag_moved_ = false;
    drag_start_ = {};
    drag_offset_ = {};
  }

private:
  HWND list_{};
  HWND window_{};
  bool dragging_{};
  bool drag_moved_{};
  POINT drag_start_{};
  POINT drag_offset_{};
  std::size_t controls_panel_{SIZE_MAX};
};

// Owns persisted/staged appearance state and every GDI brush derived from it.
// While the appearance dialog is open, staged values are the only preview source.
class AppearanceEditor final {
public:
  [[nodiscard]] layerc::platform::EditorTheme &theme() noexcept { return theme_; }
  [[nodiscard]] layerc::platform::EditorTheme theme() const noexcept {
    return theme_;
  }
  [[nodiscard]] std::uint32_t &accent() noexcept { return accent_; }
  [[nodiscard]] std::uint32_t accent() const noexcept { return accent_; }
  [[nodiscard]] AppearanceDraft &draft() noexcept { return draft_; }
  [[nodiscard]] const AppearanceDraft &draft() const noexcept { return draft_; }
  [[nodiscard]] bool &open() noexcept { return open_; }
  [[nodiscard]] bool open() const noexcept { return open_; }
  [[nodiscard]] auto &brushes() noexcept { return brushes_; }
  [[nodiscard]] const auto &brushes() const noexcept { return brushes_; }
  [[nodiscard]] std::uint32_t &picker_color() noexcept { return picker_color_; }
  [[nodiscard]] std::uint32_t picker_color() const noexcept {
    return picker_color_;
  }
  [[nodiscard]] bool &picker_syncing() noexcept { return picker_syncing_; }
  [[nodiscard]] bool picker_syncing() const noexcept { return picker_syncing_; }

private:
  layerc::platform::EditorTheme theme_{
      layerc::platform::EditorTheme::copper};
  std::uint32_t accent_{0xD88E4BU};
  AppearanceDraft draft_;
  bool open_{};
  std::array<HBRUSH, static_cast<std::size_t>(
                         layerc::platform::EditorControlRole::count)>
      brushes_{};
  std::uint32_t picker_color_{};
  bool picker_syncing_{};
};

enum class TaskTargetRequest { none, live_edit, calibration };

// Owns detected-task enumeration and picker state. Target requests are a single
// enum, so calibration and live-edit requests can never be active together.
class TaskPicker final {
public:
  [[nodiscard]] HWND &window() noexcept { return window_; }
  [[nodiscard]] HWND window() const noexcept { return window_; }
  [[nodiscard]] HWND &list() noexcept { return list_; }
  [[nodiscard]] HWND list() const noexcept { return list_; }
  [[nodiscard]] DialogChrome &chrome() noexcept { return chrome_; }
  [[nodiscard]] const DialogChrome &chrome() const noexcept { return chrome_; }
  [[nodiscard]] std::vector<layerc::platform::WindowCandidate> &candidates()
      noexcept {
    return candidates_;
  }
  [[nodiscard]] const std::vector<layerc::platform::WindowCandidate> &
  candidates() const noexcept {
    return candidates_;
  }
  [[nodiscard]] bool &picking() noexcept { return picking_; }
  [[nodiscard]] bool picking() const noexcept { return picking_; }
  [[nodiscard]] bool &was_visible() noexcept { return was_visible_; }
  [[nodiscard]] bool was_visible() const noexcept { return was_visible_; }
  [[nodiscard]] TaskTargetRequest request() const noexcept { return request_; }
  [[nodiscard]] bool has_request() const noexcept {
    return request_ != TaskTargetRequest::none;
  }
  [[nodiscard]] bool awaiting_live_edit() const noexcept {
    return request_ == TaskTargetRequest::live_edit;
  }
  [[nodiscard]] bool awaiting_calibration() const noexcept {
    return request_ == TaskTargetRequest::calibration;
  }
  void request_live_edit() noexcept { request_ = TaskTargetRequest::live_edit; }
  void request_calibration() noexcept {
    request_ = TaskTargetRequest::calibration;
  }
  void clear_request() noexcept { request_ = TaskTargetRequest::none; }

private:
  HWND window_{};
  HWND list_{};
  DialogChrome chrome_;
  std::vector<layerc::platform::WindowCandidate> candidates_;
  TaskTargetRequest request_{TaskTargetRequest::none};
  bool picking_{};
  bool was_visible_{};
};

// Owns long-lived pack/diagnostics dialog state. Import/export preview data stays
// local to its modal dialog and cannot outlive the validation plan it displays.
class PackDialogs final {
public:
  [[nodiscard]] HWND &diagnostics_window() noexcept {
    return diagnostics_window_;
  }
  [[nodiscard]] HWND diagnostics_window() const noexcept {
    return diagnostics_window_;
  }
  [[nodiscard]] DialogChrome &diagnostics_chrome() noexcept {
    return diagnostics_chrome_;
  }
  [[nodiscard]] const DialogChrome &diagnostics_chrome() const noexcept {
    return diagnostics_chrome_;
  }
  [[nodiscard]] std::string &runtime_diagnostics_json() noexcept {
    return runtime_diagnostics_json_;
  }
  [[nodiscard]] const std::string &runtime_diagnostics_json() const noexcept {
    return runtime_diagnostics_json_;
  }

private:
  HWND diagnostics_window_{};
  DialogChrome diagnostics_chrome_;
  std::string runtime_diagnostics_json_;
};

// Owns the authenticated daemon channel and one optional on-window edit session.
// Session identifiers, targets, revisions, and deadlines are advanced together.
class LiveEditSession final {
public:
  [[nodiscard]] layerc::platform::NamedPipeEditorDaemonChannel &channel()
      noexcept {
    return channel_;
  }
  [[nodiscard]] const layerc::platform::NamedPipeEditorDaemonChannel &channel()
      const noexcept {
    return channel_;
  }
  [[nodiscard]] layerc::core::ProfileRevision &revision() noexcept {
    return revision_;
  }
  [[nodiscard]] layerc::core::ProfileRevision revision() const noexcept {
    return revision_;
  }
  [[nodiscard]] layerc::core::ProfileRevision &peer_revision() noexcept {
    return peer_revision_;
  }
  [[nodiscard]] layerc::core::ProfileRevision peer_revision() const noexcept {
    return peer_revision_;
  }
  [[nodiscard]] layerc::core::EditSessionId &session_id() noexcept {
    return session_id_;
  }
  [[nodiscard]] layerc::core::EditSessionId session_id() const noexcept {
    return session_id_;
  }
  [[nodiscard]] HWND &target() noexcept { return target_; }
  [[nodiscard]] HWND target() const noexcept { return target_; }
  [[nodiscard]] HWND &pending_target() noexcept { return pending_target_; }
  [[nodiscard]] HWND pending_target() const noexcept { return pending_target_; }
  [[nodiscard]] ULONGLONG &pending_deadline() noexcept {
    return pending_deadline_;
  }
  [[nodiscard]] ULONGLONG pending_deadline() const noexcept {
    return pending_deadline_;
  }
  [[nodiscard]] bool &direct_drag_enabled() noexcept {
    return direct_drag_enabled_;
  }
  [[nodiscard]] bool direct_drag_enabled() const noexcept {
    return direct_drag_enabled_;
  }
  [[nodiscard]] bool &direct_drag_notice_shown() noexcept {
    return direct_drag_notice_shown_;
  }
  [[nodiscard]] bool direct_drag_notice_shown() const noexcept {
    return direct_drag_notice_shown_;
  }
  [[nodiscard]] ULONGLONG &last_drag_send() noexcept { return last_drag_send_; }
  [[nodiscard]] ULONGLONG last_drag_send() const noexcept {
    return last_drag_send_;
  }
  [[nodiscard]] ULONGLONG &last_heartbeat_send() noexcept {
    return last_heartbeat_send_;
  }
  [[nodiscard]] ULONGLONG last_heartbeat_send() const noexcept {
    return last_heartbeat_send_;
  }
  [[nodiscard]] UINT &timer_interval() noexcept { return timer_interval_; }
  [[nodiscard]] UINT timer_interval() const noexcept { return timer_interval_; }
  [[nodiscard]] bool &package_authenticated() noexcept {
    return package_authenticated_;
  }
  [[nodiscard]] bool package_authenticated() const noexcept {
    return package_authenticated_;
  }

  void await_target(const HWND target, const ULONGLONG deadline) noexcept {
    pending_target_ = target;
    pending_deadline_ = deadline;
  }
  [[nodiscard]] HWND take_pending_target() noexcept {
    const HWND result = pending_target_;
    pending_target_ = nullptr;
    pending_deadline_ = 0;
    return result;
  }
  void clear_pending_target() noexcept {
    pending_target_ = nullptr;
    pending_deadline_ = 0;
  }
  void begin_session(const HWND target,
                     const layerc::core::EditSessionId session_id) noexcept {
    clear_pending_target();
    target_ = target;
    session_id_ = session_id == 0 ? 1 : session_id;
    peer_revision_ = 0;
    direct_drag_enabled_ = false;
    direct_drag_notice_shown_ = false;
  }
  void clear_session() noexcept {
    session_id_ = 0;
    peer_revision_ = 0;
    target_ = nullptr;
    clear_pending_target();
    direct_drag_enabled_ = false;
    direct_drag_notice_shown_ = false;
  }

private:
  layerc::platform::NamedPipeEditorDaemonChannel channel_{
      layerc::platform::NamedPipeEditorDaemonChannel::Role::client};
  layerc::core::ProfileRevision revision_{};
  layerc::core::ProfileRevision peer_revision_{};
  layerc::core::EditSessionId session_id_{};
  HWND target_{};
  HWND pending_target_{};
  ULONGLONG pending_deadline_{};
  ULONGLONG last_drag_send_{};
  ULONGLONG last_heartbeat_send_{};
  UINT timer_interval_{100};
  bool direct_drag_enabled_{};
  bool direct_drag_notice_shown_{};
  bool package_authenticated_{};
};

} // namespace layerc::editor
