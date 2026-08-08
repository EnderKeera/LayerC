#pragma once

#include "layerc/core/profile.hpp"
#include "layerc/core/live_edit.hpp"
#include "layerc/platform/live_channel.hpp"
#include "layerc/platform/windows.hpp"
#include "editor_history.hpp"

#include <commctrl.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::editor {

struct RoleColors {
  COLORREF fill{};
  COLORREF text{};
  COLORREF border{};
  COLORREF hover{};
  COLORREF selection{};
  COLORREF disabled{};
};

struct ProfileCheckboxMetrics {
  int image_size{};
  int inset{};
  int pen_width{};
};

void set_owner_draw_checkbox(HWND control, bool checked);
[[nodiscard]] bool owner_draw_checkbox_checked(HWND control);
[[nodiscard]] ProfileCheckboxMetrics profile_checkbox_metrics(UINT dpi);
[[nodiscard]] RoleColors resolved_colors(
    layerc::platform::EditorTheme theme, std::uint32_t accent,
    layerc::platform::EditorControlRole role);
[[nodiscard]] COLORREF color_ref(std::uint32_t color);
[[nodiscard]] std::uint32_t rgb_value(COLORREF color);
[[nodiscard]] std::wstring to_wide(std::string_view text);
[[nodiscard]] std::string to_utf8(std::wstring_view text);
[[nodiscard]] std::wstring control_text(HWND control);
void set_text(HWND control, std::wstring_view text);
void enable_native_dark_mode();
void set_dark_title_bar(HWND window);
void style_native_dialog_children(HWND dialog, HFONT font);
[[nodiscard]] bool start_daemon();
[[nodiscard]] bool recycle_profile_file(const std::filesystem::path &path);
[[nodiscard]] std::string safe_identifier(
    std::wstring_view text, std::string_view fallback = "profile");
[[nodiscard]] layerc::core::GameProfile make_new_profile();
[[nodiscard]] std::wstring
profile_list_label(const layerc::core::GameProfile &profile);
[[nodiscard]] std::string unique_panel_id(
    std::string_view base, const std::vector<layerc::core::Panel> &existing,
    const std::vector<layerc::core::Panel> *generated = nullptr);
[[nodiscard]] std::wstring
profile_error_text(const layerc::core::ProfileError &error);
[[nodiscard]] std::uint32_t parse_uint(std::wstring_view text,
                                       std::uint32_t fallback);
[[nodiscard]] std::wstring color_label(const wchar_t *label,
                                        std::uint32_t value);
class EditorWindow {
public:
  int run(HINSTANCE instance, int show_command);

private:
  static constexpr UINT_PTR autosave_timer = 10;
  static constexpr UINT_PTR live_channel_timer = 11;
  struct ProfileEntry { std::filesystem::path path; std::wstring label; bool enabled{true}; };

  static INT_PTR CALLBACK dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static INT_PTR CALLBACK task_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static INT_PTR CALLBACK options_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static INT_PTR CALLBACK appearance_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static INT_PTR CALLBACK color_dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK preview_subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  bool handle_binding_capture_message(const MSG &message);

  [[nodiscard]] RoleColors colors(layerc::platform::EditorControlRole role) const {
    return resolved_colors(theme_, accent_, role);
  }
  void initialize(HWND);
  void initialize_tasks(HWND);
  void initialize_options(HWND);
  void initialize_appearance(HWND);
  void style_dialog_children(HWND) const;
  void style_list_view(HWND, bool hide_header) const;
  void rebuild_profile_check_images();
  void rebuild_brushes();
  void refresh_theme();
  void layout();
  void layout_tasks(HWND);
  void set_status(std::wstring_view text) { set_text(status_, text); }
  [[nodiscard]] HBRUSH brush(layerc::platform::EditorControlRole role) const {
    return brushes_[static_cast<std::size_t>(role)];
  }

  void refresh_profile_controls();
  void refresh_cooldown_list(int selection = 0);
  [[nodiscard]] layerc::core::CooldownDefinition *selected_cooldown();
  void refresh_binding_controls();
  void begin_binding_capture();
  void cancel_binding_capture(std::wstring_view status = {});
  void assign_binding_key(LPARAM key_data);
  void refresh_bar_list(std::size_t preferred_panel = SIZE_MAX);
  void refresh_bar_controls();
  void sync_profile_from_controls();
  [[nodiscard]] layerc::core::Panel *selected_bar();
  [[nodiscard]] const layerc::core::Panel *selected_bar() const;
  [[nodiscard]] bool validate_profile();
  [[nodiscard]] bool confirm_unsaved_changes();
  void new_profile(bool confirm = true);
  void reload_profile_list(const std::filesystem::path &select_path = {}, bool open_selection = true);
  void open_profile_row(int row, bool confirm);
  void save_profile_enabled(int row, bool enabled);
  [[nodiscard]] bool choose_save_path(std::filesystem::path &path);
  [[nodiscard]] bool save_profile(bool force_save_as);
  void delete_profile();
  void open_profiles_folder();
  void show_file_menu();
  void edit_on_window();
  void begin_edit_on_window(HWND target);
  void end_edit_on_window(std::wstring_view status = {},
                          bool notify_daemon = true);
  void edit_selected_task();

  void refresh_tasks();
  [[nodiscard]] HWND selected_task_window() const;
  [[nodiscard]] const layerc::platform::WindowCandidate *selected_task() const;
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
  void update_color_picker_from_rgb(HWND);
  void update_color_picker_from_hex(HWND);
  [[nodiscard]] bool draw_color_picker_item(const DRAWITEMSTRUCT &) const;

  [[nodiscard]] double preview_aspect_ratio() const;
  [[nodiscard]] RECT preview_canvas() const;
  [[nodiscard]] RECT bar_preview_rect(const layerc::core::Panel &, const RECT &) const;
  void paint_preview(HWND);
  bool handle_preview_message(HWND, UINT, WPARAM, LPARAM);
  void update_dragged_bar(POINT, bool final = false);
  void finalize_dragged_bar();

  enum class ChangeKind { text, immediate, drag };
  void record_profile_change(ChangeKind kind);
  void schedule_autosave(bool immediate);
  [[nodiscard]] bool flush_autosave();
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
  void reset_history();
  void sync_current_profile_entry();

  void open_options();
  void apply_options(HWND);
  [[nodiscard]] layerc::platform::EditorControlRole appearance_role(HWND) const;
  static std::uint32_t *appearance_value(layerc::platform::EditorAppearanceOverrides &, std::uint32_t);
  [[nodiscard]] COLORREF appearance_color(HWND, std::uint32_t) const;
  void refresh_appearance_controls(HWND);
  void choose_appearance_color(HWND, std::uint32_t);
  void reset_appearance(HWND);

  void draw_button(const DRAWITEMSTRUCT &, layerc::platform::EditorControlRole,
                   std::optional<COLORREF> fill = std::nullopt) const;
  void draw_list_box_item(const DRAWITEMSTRUCT &) const;
  void draw_profile_item(const DRAWITEMSTRUCT &) const;
  void draw_checkbox(const DRAWITEMSTRUCT &) const;
  void draw_appearance_preview(const DRAWITEMSTRUCT &, HWND) const;
  LRESULT color_message(UINT, WPARAM, LPARAM) const;
  LRESULT handle_main_message(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle_task_message(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle_options_message(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle_appearance_message(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle_color_picker_message(HWND, UINT, WPARAM, LPARAM);

  HINSTANCE instance_{};
  HWND dialog_{};
  HWND task_dialog_{};
  HWND profile_list_{};
  HWND cooldown_list_{};
  HWND bar_list_{};
  HWND preview_{};
  HWND status_{};
  HWND application_path_{};
  HWND task_list_{};
  HIMAGELIST profile_check_images_{};
  HFONT font_{};
  HFONT task_font_{};
  std::array<HBRUSH, static_cast<std::size_t>(layerc::platform::EditorControlRole::count)> brushes_{};
  std::vector<ProfileEntry> entries_;
  std::vector<layerc::platform::WindowCandidate> task_candidates_;
  layerc::core::GameProfile profile_;
  std::filesystem::path current_path_;
  layerc::platform::EditorTheme theme_{layerc::platform::EditorTheme::copper};
  std::uint32_t accent_{0xD88E4BU};
  bool capture_exclusion_{true};
  bool dirty_{};
  bool syncing_{};
  bool picking_{};
  bool task_was_visible_{};
  bool dragging_bar_{};
  bool bar_drag_moved_{};
  POINT bar_drag_start_{};
  POINT bar_drag_offset_{};
  bool capturing_binding_{};
  std::uint32_t picker_color_{};
  bool picker_syncing_{};
  std::size_t bar_controls_panel_{SIZE_MAX};
  layerc::platform::NamedPipeEditorDaemonChannel live_channel_{
      layerc::platform::NamedPipeEditorDaemonChannel::Role::client};
  layerc::core::ProfileRevision revision_{};
  layerc::core::ProfileRevision peer_revision_{};
  layerc::core::EditSessionId edit_session_id_{};
  HWND edit_target_{};
  HWND pending_edit_target_{};
  ULONGLONG pending_edit_deadline_{};
  bool direct_drag_enabled_{};
  bool direct_drag_notice_shown_{};
  bool awaiting_edit_target_{};
  ULONGLONG last_live_drag_send_{};
  ULONGLONG last_heartbeat_send_{};
  UINT live_timer_interval_{100};
  EditorProfileHistory history_;
};

} // namespace layerc::editor
