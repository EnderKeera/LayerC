#pragma once

#include "editor_accessibility.hpp"
#include "editor_session.hpp"
#include "editor_menus.hpp"
#include "automation_editor.hpp"
#include "visual_editor.hpp"

#include <memory>

#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Widget.H>
#include <optional>
#include <string>

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

class EditorApp;
class PanelPreview final : public Fl_Widget {
public:
  PanelPreview(int x, int y, int width, int height, EditorApp &app);
  void draw() override;
  int handle(int event) override;
private:
  EditorApp &app_;
  std::string dragging_;
};

enum class EditorCommand {
  new_profile, duplicate, delete_profile, toggle_enabled, save, undo, redo,
  refresh, import_profiles, export_json, export_pack,
  add_cooldown, delete_cooldown, add_panel, delete_panel,
  add_keyboard, add_mouse, capture_binding, delete_binding,
  profile_page, cooldown_page, binding_page, panel_page, automation_page, visual_page, close
};

// The toolkit frontend owns only controls and the local, possibly invalid
// field buffer. EditorSession owns accepted edits, history and store effects.
class EditorApp final {
public:
  EditorSession session;
  EditorWindow window{*this, 1080, 790, "LayerE — Linux X11 editor"};
  Fl_Scroll *content{};
  Fl_Hold_Browser *profiles{}, *cooldowns{}, *panels{}, *bindings{};
  Fl_Tabs *tabs{};
  Fl_Group *profile_tab{}, *cooldown_tab{}, *binding_tab{}, *panel_tab{};
  Fl_Group *cooldown_fields{}, *panel_fields{};
  Fl_Input *name{}, *executable{}, *executable_path{}, *window_class{}, *window_title{};
  Fl_Check_Button *enabled{};
  Fl_Input *cooldown_name{}, *duration{}, *charges{};
  Fl_Choice *recovery{}, *retrigger{};
  Fl_Input *panel_name{}, *anchor_x{}, *anchor_y{}, *bar_width{}, *bar_height{};
  Fl_Input *fill{}, *background{}, *border{}, *text_color{};
  Fl_Choice *layout{}, *style{}, *member{}, *binding_target{}, *mouse_button{};
  Fl_Check_Button *member_enabled{}, *show_text{}, *hide_ready{};
  Fl_Input *key_code{};
  Fl_Multiline_Output *status{}, *daemon_status{}, *module_status{}, *profile_id{};
  Fl_Box *accessibility_status{};
  Fl_Button *undo_button{}, *redo_button{};
  PanelPreview *preview{};
  std::unique_ptr<EditorMenus> menus;
  std::unique_ptr<LinuxAutomationEditor> automation;
  std::unique_ptr<LinuxVisualEditor> visual;
  // Declared last: it releases its widget trackers before the widgets go.
  std::unique_ptr<EditorAccessibility> accessibility;

  void build();
  void refresh();
  void fill_lists();
  void refresh_forms();
  void report(std::string text);
  void query_status();
  void module_fields_changed(bool pending);
  void field_changed(Fl_Widget *widget);
  void selection_changed(Fl_Widget *widget);
  void tab_changed();
  [[nodiscard]] bool apply_fields(bool text_change = false);
  [[nodiscard]] bool save();
  [[nodiscard]] bool may_leave();
  [[nodiscard]] bool command_enabled(EditorCommand action) const;
  void command(EditorCommand action);
  void import_profiles();
  void export_profile(bool pack);
  [[nodiscard]] std::optional<std::size_t> cooldown_index() const;
  [[nodiscard]] std::optional<std::size_t> panel_index() const;
  [[nodiscard]] const std::string &selected_panel() const { return panel_id_; }

private:
  std::string cooldown_id_, panel_id_, status_text_, daemon_text_, title_text_;
  int binding_index_{-1};
  int member_index_{};
  Fl_Widget *last_field_{};
  Fl_Group *active_tab_{};
  bool pending_fields_{};
  void changed(const std::optional<std::string> &error);
  friend class PanelPreview;
};

} // namespace layerc::editor
