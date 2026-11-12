#pragma once

#include "appearance_contrast.hpp"
#include "appearance_draft.hpp"
#include "binding_capture.hpp"
#include "dialog_layout.hpp"
#include "editor_util.hpp"
#include "live_edit_session.hpp"

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
#include <string_view>
#include <utility>
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
  // Why each disabled button is unavailable (DesignSystem.md, Button): the
  // line under it, its tooltip and its accessible description.
  std::vector<std::pair<int, std::wstring>> button_reasons;
  bool advanced_visible{};
  bool syncing{};
  // Set while layout() runs, so lines it sizes don't re-enter it.
  bool laying_out{};
};

// Records why `button` is disabled (empty: it isn't) and gives the reason to
// screen readers as the button's description. Returns whether it changed.
bool set_button_reason(EditorShell &shell, int button, std::wstring_view reason);
[[nodiscard]] std::wstring_view button_reason(const EditorShell &shell,
                                              int button);
// A disabled button takes no mouse input, so its reason tooltip belongs to
// the window over the button's rectangle; call after the buttons move.
void place_button_reason_tools(const EditorShell &shell);

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
    pending_anchor.reset();
  }

  // Transient preview coordinates for one panel: UI projection state only, not
  // a document value, dirty flag or history entry.
  struct PendingAnchor {
    std::string panel_id;
    double x{};
    double y{};
  };
  std::optional<PendingAnchor> pending_anchor;

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
  // What Choose color measures its color against; empty shows no line.
  void set_picker_targets(std::vector<ContrastTarget> targets) {
    picker_targets_ = std::move(targets);
  }
  [[nodiscard]] const std::vector<ContrastTarget> &picker_targets() const noexcept {
    return picker_targets_;
  }
  [[nodiscard]] bool picker_syncing() const noexcept { return picker_syncing_; }
  [[nodiscard]] EditorShell::SyncScope picker_sync_scope() noexcept {
    return EditorShell::SyncScope(picker_syncing_);
  }

private:
  layerc::platform::EditorTheme theme_{
      layerc::platform::EditorTheme::copper};
  std::uint32_t accent_{layerc::platform::copper_accent};
  AppearanceDraft draft_;
  bool open_{};
  std::array<HBRUSH, static_cast<std::size_t>(
                         layerc::platform::EditorControlRole::count)>
      brushes_{};
  std::uint32_t picker_color_{};
  std::vector<ContrastTarget> picker_targets_;
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

} // namespace layerc::editor
