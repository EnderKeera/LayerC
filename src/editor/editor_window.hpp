#pragma once

#include "appearance_draft.hpp"
#include "automation_editor.hpp"
#include "dialog_layout.hpp"
#include "editor_history.hpp"
#include "editor_state.hpp"
#include "editor_profile_fields.hpp"
#include "editor_text.hpp"
#include "editor_util.hpp"
#include "profile_document.hpp"
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

class EditorWindow : private AutomationHost {
public:
  EditorWindow();
  ~EditorWindow();

  int run(HINSTANCE instance, int show_command);

private:
  [[nodiscard]] ProfileDocument &document() noexcept override {
    return document_;
  }
  void mark_changed(const DocumentChangeKind kind) override {
    record_profile_change(kind);
  }
  [[nodiscard]] HINSTANCE instance() const noexcept override {
    return shell_.instance();
  }
  [[nodiscard]] HWND owner_window() const noexcept override {
    return shell_.window();
  }
  [[nodiscard]] HFONT owner_font() const noexcept override {
    return shell_.font();
  }
  [[nodiscard]] bool capture_exclusion_enabled() const noexcept override {
    return shell_.capture_exclusion();
  }
  void draw_owner_list_item(const DRAWITEMSTRUCT &item) const override {
    draw_list_box_item(item);
  }
  [[nodiscard]] std::optional<LRESULT>
  owner_theme_message(const HWND window, const UINT message,
                      const WPARAM wparam, const LPARAM lparam) const override {
    return dialog_theme_message(window, message, wparam, lparam);
  }
  [[nodiscard]] bool live_connected() const noexcept override {
    return live_edit_.channel().connected();
  }
  [[nodiscard]] bool live_authenticated() const noexcept override {
    return live_edit_.package_authenticated();
  }
  void advance_live_revision() noexcept override { live_edit_.advance_revision(); }
  [[nodiscard]] layerc::core::ProfileRevision
  live_revision() const noexcept override {
    return live_edit_.revision();
  }
  void send_live(const layerc::core::LiveMessage &message) override {
    (void)live_edit_.channel().send(message);
  }

  static constexpr UINT_PTR autosave_timer = 10;
  static constexpr UINT_PTR live_channel_timer = 11;

  static INT_PTR CALLBACK export_pack_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static INT_PTR CALLBACK import_pack_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static INT_PTR CALLBACK diagnostics_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static INT_PTR CALLBACK hid_binding_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK preview_subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR,
                                           DWORD_PTR);
  bool handle_binding_capture_message(const MSG &message);

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
  void style_dialog_children(HWND) const;
  void style_list_view(HWND, bool hide_header) const;
  void rebuild_profile_check_hit_images();
  void rebuild_brushes();
  void refresh_theme();
  void layout();
  void layout_tasks(HWND);
  void set_status(std::wstring_view text) { set_text(shell_.status(), text); }
  [[nodiscard]] HBRUSH brush(layerc::platform::EditorControlRole role) const {
    return appearance_editor_.brushes()[static_cast<std::size_t>(role)];
  }

  void refresh_profile_controls();
  void refresh_cooldown_list(int selection = 0);
  void refresh_selected_cooldown_label();
  [[nodiscard]] layerc::core::CooldownDefinition *selected_cooldown();
  void refresh_binding_controls();
  void load_current_visual_profile();
  void load_current_automation_profile();
  void refresh_visual_controls();
  void calibrate_visual();
  void calibrate_selected_task();
  bool launch_calibration(HWND target);
  [[nodiscard]] bool merge_calibration_result(std::string_view visual_json);
  [[nodiscard]] bool save_visual_profile();
  void report_visual_asset_gc(
      const layerc::platform::VisualAssetGcReport &report);
  void set_visual_assignment_enabled(bool enabled);
  void remove_visual_assignment();
  // Single document-commit policy shared by explicit save, autosave, and
  // immediate visual actions. Loaded state (absent/valid/recovered/invalid)
  // is separate from mutation intent: an unreadable module (invalid with no
  // parsed profile) must block the commit so a nullopt never becomes an
  // explicit removal request that discards recovery bytes and backups.
  [[nodiscard]] bool can_commit_document_bundle() const;
  [[nodiscard]] layerc::platform::ProfileWriteResult
  commit_document_bundle(const std::filesystem::path &destination);
  [[nodiscard]] std::wstring write_failure_message() const;
  // serialize → parse → reassign, keeping sidecar profile ids aligned.
  // Nullopt when the profile round-trips cleanly.
  [[nodiscard]] std::optional<layerc::core::ProfileError> normalize_for_commit();
  // Shared pipeline: draft → sync → normalize → upgrade → can_commit → write.
  [[nodiscard]] CommitResult
  commit_bundle(const std::filesystem::path &path,
                const CommitOptions &options = {});
  // Staged one-field edits (row enablement): normalize the prepared bundle and
  // write it without re-reading document controls.
  [[nodiscard]] CommitResult
  commit_bundle(const std::filesystem::path &path,
                layerc::core::GameProfile &profile,
                std::optional<layerc::core::VisualProfile> &visual,
                std::optional<layerc::core::AutomationProfile> &automation,
                bool upgrade_needed, const CommitOptions &options = {});
  // Shared postlude: dirty/legacy/rebase/timer/notify. A path that is not the
  // open document only notifies so a row edit cannot clear its dirty state.
  void mark_committed(const CommitOptions &options = {});
  void mark_committed(const std::filesystem::path &path,
                      const layerc::core::GameProfile &profile,
                      const CommitOptions &options = {});
  void begin_binding_capture();
  void capture_hid_binding();
  void cancel_binding_capture(std::wstring_view status = {});
  void assign_binding_chord(std::span<const std::uint16_t> scan_codes);
  void refresh_bar_list(std::size_t preferred_panel = SIZE_MAX);
  void refresh_bar_controls();
  void sync_profile_from_controls();
  [[nodiscard]] layerc::core::Panel *selected_bar();
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
  void apply_task(const layerc::platform::WindowCandidate &candidate);
  void use_selected_task();
  void show_tasks();
  void begin_picker();
  void finish_picker();
  void cancel_picker();

  void add_cooldown();
  void remove_cooldown();
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
  [[nodiscard]] RECT bar_preview_rect(const layerc::core::Panel &,
                                      const RECT &) const;
  void paint_preview(HWND);
  bool handle_preview_message(HWND, UINT, WPARAM, LPARAM);
  void update_dragged_bar(POINT, bool final = false);
  void finalize_dragged_bar();

  using ChangeKind = DocumentChangeKind;
  void record_profile_change(ChangeKind kind);
  void schedule_autosave(bool immediate);
  [[nodiscard]] bool flush_autosave(bool allow_upgrade_prompt = true);
  [[nodiscard]] bool draft_controls_valid() const;
  void notify_profile_committed(const std::filesystem::path &path,
                                const layerc::core::GameProfile &profile);
  void poll_live_channel();
  void handle_live_message(const layerc::core::LiveMessage &message);
  void send_live_anchor(bool final);
  void send_live_profile();
  void select_bar_by_id(std::string_view panel_id);
  void set_direct_drag_enabled(bool enabled, bool show_notice);
  void undo();
  void redo();
  void apply_history_snapshot(EditorProfileSnapshot snapshot);
  void reset_history();
  void sync_current_profile_entry();

  void open_options();
  void open_appearance();
  void apply_options(HWND);
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
  void draw_list_box_item(const DRAWITEMSTRUCT &) const;
  void draw_profile_item(const DRAWITEMSTRUCT &) const;
  void draw_checkbox(const DRAWITEMSTRUCT &) const;
  void draw_appearance_preview(const DRAWITEMSTRUCT &, HWND) const;
  LRESULT color_message(UINT, WPARAM, LPARAM) const;
  LRESULT erase_dialog_background(HWND, WPARAM) const;
  [[nodiscard]] std::optional<LRESULT>
  dialog_theme_message(HWND, UINT, WPARAM, LPARAM) const;
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
  std::unique_ptr<AutomationEditor> automation_editor_;
  std::optional<layerc::platform::ProfileWriteError> last_write_error_;
};

} // namespace layerc::editor
