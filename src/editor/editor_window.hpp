#pragma once

#include "appearance_draft.hpp"
#include "automation_editor.hpp"
#include "cooldown_editor.hpp"
#include "dialog_layout.hpp"
#include "editor_history.hpp"
#include "editor_state.hpp"
#include "editor_profile_fields.hpp"
#include "editor_text.hpp"
#include "editor_util.hpp"
#include "profile_document.hpp"
#include "visual_editor.hpp"
#include "layerc/core/automation.hpp"
#include "layerc/core/calibration.hpp"
#include "layerc/core/live_edit.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/visual.hpp"
#include "layerc/platform/editor_appearance.hpp"
#include "layerc/platform/live_channel.hpp"
#include "layerc/platform/native_theme.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/window_query.hpp"

#include <commctrl.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::platform {
struct VisualAssetGcReport;
}

namespace layerc::editor {

class EditorWindow final : private DialogTheme {
public:
  EditorWindow();
  ~EditorWindow();

  int run(HINSTANCE instance, int show_command);

private:
  friend struct EditorWindowTestAccess;
  friend struct PanelItemDialogState;

  static constexpr UINT_PTR autosave_timer = 10;
  static constexpr UINT_PTR live_channel_timer = 11;
  // Coalesces live profile sends (about 30 Hz) while the user types or drags.
  static constexpr UINT_PTR live_profile_timer = 12;
  static constexpr UINT live_profile_interval_ms = 33;

  static INT_PTR CALLBACK export_pack_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static INT_PTR CALLBACK import_pack_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static INT_PTR CALLBACK diagnostics_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK preview_subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR,
                                           DWORD_PTR);
  // Owns mouse toggling of profile enablement: a click inside a row's
  // checkbox cell flips the check state (reported once through
  // LVN_ITEMCHANGED, like Space) and never reaches the list view.
  static LRESULT CALLBACK profile_list_subclass(HWND, UINT, WPARAM, LPARAM,
                                                UINT_PTR, DWORD_PTR);

  [[nodiscard]] RoleColors
  colors(layerc::platform::EditorControlRole role) const {
    const auto &appearance = appearance_editor_.draft().staged();
    return layerc::platform::native_theme_colors(
        appearance_editor_.open() ? appearance.theme : appearance_editor_.theme(),
        appearance_editor_.open() ? appearance.accent : appearance_editor_.accent(), role,
        appearance.overrides[static_cast<std::size_t>(role)]);
  }
  void initialize(HWND);
  void initialize_tasks(HWND);
  void initialize_options(HWND);
  void initialize_appearance(HWND);
  void style_dialog_children(HWND) const override;
  void style_list_view(HWND, bool hide_header) const;
  void rebuild_profile_check_hit_images();
  void rebuild_brushes();
  // Hands the staged edit/toolbar colors to the platform control subclasses
  // (field frames, menu bar, button parent fill).
  void publish_chrome_palette() const;
  void refresh_theme();
  // Recreates the body, heading and title fonts for the current DPI and text
  // size, restyles the main window's children and lays it out again.
  void reload_fonts();
  void layout();
  // Semibold section headings and the 20 px profile title.
  void apply_heading_fonts();
  // Main-window behaviors (editor_shell.cpp): tooltips, numeric stepping,
  // context menus, list shortcuts, the draft row, title, placement, status.
  void install_shell_helpers();
  [[nodiscard]] std::optional<LRESULT> handle_shell_notify(const NMHDR &header);
  void browse_application();
  void apply_application_text();
  void refresh_title();
  void sync_draft_row();
  void focus_field(int id);
  void show_context_menu(HWND source, POINT screen);
  bool handle_list_key(const MSG &message);
  void duplicate_profile();
  void set_advanced_visible(bool visible, bool persist);
  void set_text_scale(int percent);
  void update_view_menu(HMENU popup) const;
  void save_window_placement() const;
  bool restore_window_placement(int show_command);
  void refresh_connection_status();
  void paint_status_strip(HDC dc) const;
  void show_keyboard_shortcuts();
  void show_about();
  // Selects the bar that shows the selected cooldown.
  void follow_cooldown_bar();
  void layout_tasks(HWND);
  void set_status(std::wstring_view text) { set_text(shell_.status, text); }
  [[nodiscard]] HBRUSH brush(layerc::platform::EditorControlRole role) const {
    return appearance_editor_.brushes()[static_cast<std::size_t>(role)];
  }

  void refresh_profile_controls();
  // Routes Calibrate on the task picker to the visual editor.
  void calibrate_selected_task();
  [[nodiscard]] std::wstring write_failure_message() const;
  // serialize → parse → reassign, keeping sidecar profile ids aligned.
  // Nullopt when the profile round-trips cleanly.
  [[nodiscard]] std::optional<layerc::core::ProfileError> normalize_for_commit();
  // The one commit pipeline: normalize → unreadable-module gate → upgrade
  // confirmation → write. `bundle` is left normalized.
  [[nodiscard]] CommitResult commit_bundle(const std::filesystem::path &path,
                                           ProfileBundle &bundle,
                                           const CommitOptions &options = {});
  // Commits the open document: draft controls → sync → commit_bundle, then
  // adopts the normalized profile.
  [[nodiscard]] CommitResult
  commit_document(const std::filesystem::path &path,
                  const CommitOptions &options = {},
                  const std::function<void(ProfileBundle &)> &candidate = {});
  // Refreshes UI/timers and notifies after an accepted commit; no document
  // mutation. A different row only notifies, preserving the open document.
  void mark_committed(const CommitOptions &options = {});
  void mark_committed(const std::filesystem::path &path,
                      const layerc::core::GameProfile &profile,
                      const CommitOptions &options = {});
  void refresh_bar_list(std::size_t preferred_panel = SIZE_MAX);
  void refresh_bar_controls();
  void refresh_panel_items(std::size_t preferred = SIZE_MAX);
  void refresh_item_actions();
  [[nodiscard]] bool handle_item_command(int id, int notification);
  void open_panel_item_dialog(bool editing);
  static INT_PTR CALLBACK panel_item_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  [[nodiscard]] bool commit_panel_item(
      std::size_t panel, std::optional<std::size_t> index,
      layerc::core::OverlayItem item,
      std::optional<layerc::core::Milliseconds> preset_interval,
      std::string &error);
  [[nodiscard]] layerc::core::PanelPresentation
  preview_presentation(const layerc::core::Panel &) const;
  void refresh_position_hint();
  [[nodiscard]] const layerc::core::Panel *selected_bar() const;
  [[nodiscard]] bool validate_profile();
  [[nodiscard]] bool confirm_v2_upgrade();
  [[nodiscard]] bool confirm_unsaved_changes();
  void new_profile(bool confirm = true);
  void reload_profile_list(const std::filesystem::path &select_path = {},
                           bool open_selection = true);
  void open_profile_row(int row, bool confirm);
  void save_profile_enabled(int row, bool enabled);
  [[nodiscard]] bool choose_save_path(std::filesystem::path &path);
  [[nodiscard]] bool save_profile(bool force_save_as);
  void delete_profile();
  void open_profiles_folder();
  void export_profile_pack();
  void import_profile_pack();
  void open_runtime_diagnostics();
  void request_runtime_diagnostics();
  void refresh_runtime_diagnostics_dialog();
  void edit_on_window();
  void begin_edit_on_window(HWND target);
  void end_edit_on_window(std::wstring_view status = {},
                          bool notify_daemon = true);
  void edit_selected_task();

  void refresh_tasks();
  [[nodiscard]] HWND selected_task_window() const;
  [[nodiscard]] const layerc::platform::WindowCandidate *selected_task() const;
  [[nodiscard]] const layerc::platform::WindowCandidate *
  require_selected_task(bool live_window_required = true);
  void update_task_detail();
  [[nodiscard]] const wchar_t *empty_task_list_text() const;
  void apply_task(const layerc::platform::WindowCandidate &candidate);
  void use_selected_task();
  void show_tasks();
  void begin_picker();
  void finish_picker();
  void cancel_picker();

  void split_independent_bars();
  void choose_bar_color(int id);
  [[nodiscard]] bool choose_color(HWND owner, std::uint32_t &value);
  void initialize_color_picker(HWND);
  void refresh_color_picker(HWND);
  void refresh_color_picker_rgb(HWND);
  void update_color_picker_from_rgb(HWND);
  void update_color_picker_from_hex(HWND);
  [[nodiscard]] bool draw_color_picker_item(const DRAWITEMSTRUCT &) const;

  [[nodiscard]] SIZE preview_target_size() const;
  [[nodiscard]] RECT preview_canvas() const;
  // Height of the "Actual size" strip under the canvas (0 when hidden).
  [[nodiscard]] int preview_actual_size_height() const;
  // The panel's preview projection: committed values, with a pending drag
  // anchor applied when it belongs to this panel.
  [[nodiscard]] layerc::core::Panel
  preview_panel(const layerc::core::Panel &panel) const;
  // Applies a pending anchor as ONE document transaction.
  [[nodiscard]] bool commit_pending_anchor(bool publish_final);
  [[nodiscard]] RECT bar_preview_rect(const layerc::core::Panel &,
                                      const RECT &) const;
  void paint_preview(HWND);
  bool handle_preview_message(HWND, UINT, WPARAM, LPARAM);
  void update_dragged_bar(POINT, bool final = false);
  void finalize_dragged_bar();

  using ChangeKind = DocumentChangeKind;
  // One notification for an accepted change: autosave scheduling, live publish
  // and entry/title refresh only. It never touches history.
  void document_changed(DocumentChangeKind kind);
  // Routes a validated edit outcome through the one funnel.
  [[nodiscard]] bool apply_edit(const ModuleEditOutcome &outcome,
                                DocumentChangeKind kind);
  // Reads the profile controls into the document as one validated edit, with an
  // optional extra mutation inside the same candidate transaction.
  [[nodiscard]] bool apply_profile_change(
      DocumentChangeKind kind,
      const std::function<void(layerc::core::GameProfile &)> &extra = {});
  [[nodiscard]] bool apply_profile_controls(DocumentChangeKind kind);
  [[nodiscard]] bool has_unsaved_changes() const;
  void schedule_autosave(bool immediate);
  [[nodiscard]] bool flush_autosave(bool allow_upgrade_prompt = true);
  // Live edit and calibration act on the stored profile: a new draft must be
  // saved once, and pending edits are flushed first.
  [[nodiscard]] bool require_stored_profile(const wchar_t *first_save_message);
  [[nodiscard]] bool draft_controls_valid() const;
  void notify_profile_committed(const std::filesystem::path &path,
                                const layerc::core::GameProfile &profile);
  void poll_live_channel();
  void handle_live_message(const layerc::core::LiveMessage &message);
  void send_live_anchor(bool final);
  void send_live_profile();
  void flush_live_profile();
  void select_bar_by_id(std::string_view panel_id);
  void set_direct_drag_enabled(bool enabled);
  void undo();
  void redo();
  void sync_current_profile_entry();

  void open_options();
  void open_appearance();
  bool apply_options(HWND);
  [[nodiscard]] layerc::platform::EditorControlRole appearance_role(HWND) const;
  static std::uint32_t *
  appearance_value(layerc::platform::EditorAppearanceOverrides &,
                   std::uint32_t);
  [[nodiscard]] std::optional<COLORREF>
  appearance_color(HWND, std::uint32_t) const;
  void refresh_appearance_controls(HWND);
  void choose_appearance_color(HWND, std::uint32_t);
  void reset_appearance(HWND);
  void reset_all_appearance(HWND);
  [[nodiscard]] bool apply_appearance(HWND);

  void draw_button(const DRAWITEMSTRUCT &, layerc::platform::EditorControlRole,
                   std::optional<COLORREF> fill = std::nullopt) const;
  void draw_list_box_item(const DRAWITEMSTRUCT &) const override;
  void draw_profile_item(const DRAWITEMSTRUCT &) const;

  void draw_appearance_preview(const DRAWITEMSTRUCT &, HWND) const;
  LRESULT color_message(UINT, WPARAM, LPARAM) const;
  LRESULT erase_dialog_background(HWND, WPARAM) const;
  [[nodiscard]] std::optional<LRESULT>
  dialog_theme_message(HWND, UINT, WPARAM, LPARAM) const override;
  [[nodiscard]] std::optional<LRESULT> handle_main_command(WPARAM);
  [[nodiscard]] std::optional<LRESULT> handle_main_notify(LPARAM);
  [[nodiscard]] std::optional<LRESULT> handle_main_draw_item(LPARAM);
  LRESULT handle_main_message(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle_task_message(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle_options_message(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle_appearance_message(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle_color_picker_message(HWND, UINT, WPARAM, LPARAM);

  EditorShell shell_;
  ProfileDocument document_;
  CooldownEditor cooldown_editor_;
  PanelPreview panel_preview_;
  AppearanceEditor appearance_editor_;
  TaskPicker task_picker_;
  PackDialogs pack_dialogs_;
  LiveEditSession live_edit_;
  VisualEditor visual_editor_;
  bool live_profile_pending_{};
  std::unique_ptr<AutomationEditor> automation_editor_;
  std::optional<layerc::platform::ProfileWriteError> last_write_error_;
};

} // namespace layerc::editor
