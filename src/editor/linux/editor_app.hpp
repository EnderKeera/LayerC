#pragma once

#include "editor_accessibility.hpp"
#include "editor_session.hpp"
#include "editor_menus.hpp"
#include "editor_pages.hpp"
#include "automation_editor.hpp"
#include "visual_editor.hpp"
#include "live_draft_client.hpp"
#include "task_list.hpp"
#include "tasks_window.hpp"
#include "diagnostics_window.hpp"
#include "editor_appearance.hpp"
#include "status_voice.hpp"

#include <memory>

#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Widget.H>
#include <optional>
#include <string>
#include <sys/types.h>

class Fl_Box;
class Fl_Button;
class Fl_Check_Button;
class Fl_Choice;
class Fl_Group;
class Fl_Hold_Browser;
class Fl_Input;
class Fl_Multiline_Output;
class Fl_Scroll;
class Fl_Tabs;

namespace layerc::editor {

// The one schema-upgrade confirmation every save path and command shares.
[[nodiscard]] bool confirm_profile_upgrade();
[[nodiscard]] bool confirm_items_profile_upgrade();

class EditorApp;
struct SimulatorPanelPreview;
class PanelPreview final : public Fl_Widget {
public:
  PanelPreview(int x, int y, int width, int height, EditorApp &app);
  ~PanelPreview() override;
  void draw() override;
  int handle(int event) override;
private:
  friend struct EditorAppTestAccess;
  [[nodiscard]] SimulatorPanelPreview present(const core::Panel &panel,
                                             bool *simulated = nullptr) const;
  static void wake(void *data);
  EditorApp &app_;
  std::string dragging_;
  core::TimePoint epoch_{core::MonotonicClock::now()};
};

// The toolkit frontend owns only controls and the local, possibly invalid
// field buffer. EditorSession owns accepted edits, history and store effects.
class EditorApp final {
public:
  EditorApp();
  ~EditorApp();
  EditorSession session;
  // The root owns its shell widgets; each page/dialog owns its private controls.
  // Test-only access is confined to EditorAppTestAccess.
private:
  EditorWindow window{*this, 1080, 790, "LayerE — Linux X11 editor"};
  Fl_Scroll *content{};
  Fl_Hold_Browser *profiles{};
  Fl_Tabs *tabs{};
  Fl_Group *profile_tab{}, *cooldown_tab{}, *binding_tab{}, *panel_tab{};

  PanelPreview *preview{};
  std::unique_ptr<StatusPane> status_pane_;
  std::unique_ptr<ProfilePage> profile_page_;
  std::unique_ptr<CooldownPage> cooldown_page_;
  std::unique_ptr<BindingPage> binding_page_;
  std::unique_ptr<PanelPage> panel_page_;
  std::unique_ptr<EditorMenus> menus;
  std::unique_ptr<LinuxAutomationEditor> automation;
  std::unique_ptr<LinuxVisualEditor> visual;
  std::unique_ptr<DiagnosticsWindow> diagnostics; // demand-created
  std::unique_ptr<TasksWindow> tasks_window;     // demand-created
  // Declared last: it releases its widget trackers before the widgets go.
  std::unique_ptr<EditorAccessibility> accessibility;

public:
  void build();
  void show();
  [[nodiscard]] bool shown() const;
  [[nodiscard]] bool handle_shortcut(int key, int modifiers);
  void show_context_menu(EditorMenuContext context, bool has_target);
  void refresh();
  void redraw_preview();
  void fill_lists();
  void refresh_forms();
  void report(std::string text, StatusVoice voice = StatusVoice::announced);
  void query_status();
  void module_fields_changed(bool pending);
  void field_changed(Fl_Widget *widget);
  void selection_changed(Fl_Widget *widget);
  void tab_changed();
  [[nodiscard]] bool apply_fields(bool text_change = false);
  // The typed failure text of the visible field buffers, without reporting.
  [[nodiscard]] std::optional<std::string> apply_field_buffers(bool text_change = false);
  [[nodiscard]] bool save();
  [[nodiscard]] bool flush_autosave();
  [[nodiscard]] bool may_leave();
  [[nodiscard]] bool command_enabled(EditorCommand action) const;
  void command(EditorCommand action);
  void import_profiles();
  // Restores the saved editor appearance (Tools > Appearance) at startup.
  void load_appearance();
  [[nodiscard]] const EditorAppearance &appearance() const noexcept { return appearance_; }
  void export_profile(bool pack);
  [[nodiscard]] std::optional<std::size_t> cooldown_index() const;
  [[nodiscard]] std::optional<std::size_t> panel_index() const;
  [[nodiscard]] const std::string &selected_panel() const { return panel_id_; }
  // The game window the preview stands for while positioning on it.
  [[nodiscard]] bool placement_active() const;
  [[nodiscard]] const std::optional<TaskCandidate> &placement() const noexcept { return placement_; }

private:
  std::string cooldown_id_, panel_id_, status_text_, daemon_text_, title_text_;
  // The last message spoken, so a repeating failure is said once.
  std::string last_announced_;
  int binding_index_{-1};
  Fl_Widget *last_field_{};
  Fl_Group *active_tab_{};
  bool pending_fields_{};
  bool saving_{};
  std::uint64_t observed_generation_{};
  std::unique_ptr<LiveDraftClient> live_draft_;
  std::string live_draft_text_;
  std::uint64_t live_draft_sequence_{}; // latest publish; 0 when none is current
  void sync_edit_services();
  void schedule_autosave(bool text_change);
  void stop_edit_services();
  void update_daemon_status();
  static void autosave_tick(void *);
  static void live_draft_ready(int, void *);
  void end_live_draft();
  void open_profiles_folder();
  static void opener_tick(void *);
  void stop_opener();
  EditorAppearance appearance_;
  std::filesystem::path appearance_path_;
  std::string appearance_error_;
  pid_t opener_{};
  std::string opener_path_;
  // Detected Tasks and live placement (editor_placement.cpp).
  void show_tasks(const std::string &reason = {});
  void use_task(const TaskCandidate &task, bool exact_path);
  void position_command();
  void position_on(const TaskCandidate &task);
  void end_placement(const std::string &status);
  void update_position_button();
  std::optional<TaskCandidate> placement_;
  std::string placement_profile_;
  [[nodiscard]] std::string edit_status() const;
  void changed(const std::optional<std::string> &error);
  friend class PanelPreview;
  friend struct EditorCommandRegistry;
  friend struct EditorAppTestAccess;
};

} // namespace layerc::editor
