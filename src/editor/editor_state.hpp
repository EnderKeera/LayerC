#pragma once

#include "appearance_draft.hpp"
#include "binding_capture.hpp"
#include "dialog_layout.hpp"
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
  // Item text for accessibility ("name — app.exe (off)"); the row paints
  // `name` and `executable` separately with an Off tag instead.
  std::wstring label;
  std::wstring name;
  std::wstring executable;
  bool enabled{true};

  void describe(const layerc::core::GameProfile &profile);
};

// The main editor shell and its profile-list controls. The shell is bound
// once during WM_INITDIALOG; every borrowed child handle belongs to that window.
struct EditorShell final {
  class [[nodiscard]] SyncScope {
  public:
    explicit SyncScope(bool &syncing) noexcept
        : syncing_(syncing), was_syncing_(syncing) { syncing_ = true; }
    ~SyncScope() { syncing_ = was_syncing_; }
    SyncScope(const SyncScope &) = delete;
    SyncScope &operator=(const SyncScope &) = delete;

  private:
    bool &syncing_;
    bool was_syncing_;
  };
  [[nodiscard]] SyncScope sync_scope() noexcept { return SyncScope(syncing); }

  HINSTANCE instance{};
  HWND window{};
  HWND profile_list{};
  HWND application_path{};
  HWND status{};
  HFONT font{};
  // Semibold section headings and the 20 px profile title (F33).
  HFONT heading_font{};
  HFONT title_font{};
  HIMAGELIST profile_check_hit_images{};
  std::vector<ProfileEntry> profiles;
  std::wstring visual_runtime_status{L"Visual recognition: connecting"};
  // The editor stays visible to screenshots, sharing and Magnifier unless the
  // user opts out; overlay exclusion is LayerD's separate setting.
  bool capture_exclusion{false};
  // From the last layout(): the status strip's top edge and the smallest
  // client height that shows every row (0 before the first layout).
  int status_strip_top{};
  int min_client_height{};
  // Whether the last layout() had room for the preview column.
  bool preview_visible{};
  HWND tooltip{};
  std::wstring tooltip_text;
  std::wstring title_cache;
  std::wstring connection_cache;
  bool advanced_visible{};
  bool syncing{};
};

// Panel-list selection, preview drag state, and the panel whose controls are
// currently projected. A completed or cancelled drag resets all drag fields.
struct PanelPreview final {
  void begin_drag(const POINT start, const POINT offset) noexcept {
    dragging = true;
    drag_moved = false;
    drag_start = start;
    drag_offset = offset;
  }
  void mark_drag_moved() noexcept { drag_moved = true; }
  void finish_drag() noexcept {
    dragging = false;
    drag_moved = false;
    drag_start = {};
    drag_offset = {};
  }

  HWND list{};
  HWND window{};
  bool dragging{};
  bool drag_moved{};
  POINT drag_start{};
  POINT drag_offset{};
  std::size_t controls_panel{SIZE_MAX};
};

// Owns persisted/staged appearance state and every GDI brush derived from it.
// While the appearance dialog is open, staged values are the only preview source.
class AppearanceEditor final {
public:
  void set_appearance(const layerc::platform::EditorTheme theme,
                      const std::uint32_t accent) noexcept {
    theme_ = theme;
    accent_ = accent;
  }
  [[nodiscard]] layerc::platform::EditorTheme theme() const noexcept {
    return theme_;
  }
  [[nodiscard]] std::uint32_t accent() const noexcept { return accent_; }
  [[nodiscard]] AppearanceDraft &draft() noexcept { return draft_; }
  [[nodiscard]] const AppearanceDraft &draft() const noexcept { return draft_; }
  [[nodiscard]] bool open() const noexcept { return open_; }
  void begin_open() noexcept { open_ = true; }
  void close() noexcept { open_ = false; }
  [[nodiscard]] auto &brushes() noexcept { return brushes_; }
  [[nodiscard]] const auto &brushes() const noexcept { return brushes_; }
  void set_picker_color(const std::uint32_t color) noexcept {
    picker_color_ = color & 0xFFFFFFU;
  }
  [[nodiscard]] std::uint32_t picker_color() const noexcept {
    return picker_color_;
  }
  [[nodiscard]] bool picker_syncing() const noexcept { return picker_syncing_; }
  [[nodiscard]] EditorShell::SyncScope picker_sync_scope() noexcept {
    return EditorShell::SyncScope(picker_syncing_);
  }

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
  HWND window{};
  HWND list{};
  DialogChrome chrome;
  std::vector<layerc::platform::WindowCandidate> candidates;

  [[nodiscard]] bool picking() const noexcept { return picking_; }
  [[nodiscard]] bool was_visible() const noexcept { return was_visible_; }
  void begin_pick(const bool was_visible) noexcept {
    picking_ = true;
    was_visible_ = was_visible;
  }
  void end_pick() noexcept {
    picking_ = false;
    was_visible_ = false;
  }
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
  TaskTargetRequest request_{TaskTargetRequest::none};
  bool picking_{};
  bool was_visible_{};
};

// Long-lived pack/diagnostics dialog state. Import/export preview data stays
// local to its modal dialog and cannot outlive the validation plan it displays.
struct PackDialogs final {
  HWND diagnostics_window{};
  DialogChrome diagnostics_chrome;
  std::string runtime_diagnostics_json;
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
  [[nodiscard]] layerc::core::ProfileRevision revision() const noexcept {
    return revision_;
  }
  void set_revision(const layerc::core::ProfileRevision revision) noexcept {
    revision_ = revision;
  }
  void advance_revision() noexcept { ++revision_; }
  [[nodiscard]] layerc::core::ProfileRevision peer_revision() const noexcept {
    return peer_revision_;
  }
  void observe_peer(const layerc::core::ProfileRevision revision) noexcept {
    peer_revision_ = revision;
  }
  [[nodiscard]] layerc::core::EditSessionId session_id() const noexcept {
    return session_id_;
  }
  [[nodiscard]] HWND target() const noexcept { return target_; }
  [[nodiscard]] HWND pending_target() const noexcept { return pending_target_; }
  [[nodiscard]] ULONGLONG pending_deadline() const noexcept {
    return pending_deadline_;
  }
  [[nodiscard]] bool direct_drag_notice_shown() const noexcept {
    return direct_drag_notice_shown_;
  }
  [[nodiscard]] ULONGLONG last_drag_send() const noexcept {
    return last_drag_send_;
  }
  [[nodiscard]] ULONGLONG last_heartbeat_send() const noexcept {
    return last_heartbeat_send_;
  }
  [[nodiscard]] UINT timer_interval() const noexcept { return timer_interval_; }
  [[nodiscard]] bool package_authenticated() const noexcept {
    return package_authenticated_;
  }

  void set_package_authenticated(const bool authenticated) noexcept {
    package_authenticated_ = authenticated;
  }
  void set_timer_interval(const UINT interval) noexcept {
    timer_interval_ = interval;
  }
  void note_drag_send(const ULONGLONG now) noexcept { last_drag_send_ = now; }
  void note_heartbeat_send(const ULONGLONG now) noexcept {
    last_heartbeat_send_ = now;
  }
  void note_direct_drag_notice() noexcept { direct_drag_notice_shown_ = true; }
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
    direct_drag_notice_shown_ = false;
  }
  void clear_session() noexcept {
    session_id_ = 0;
    peer_revision_ = 0;
    target_ = nullptr;
    clear_pending_target();
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
  bool direct_drag_notice_shown_{};
  bool package_authenticated_{};
};

} // namespace layerc::editor
