#include "editor_app.hpp"
#include "profile_edit.hpp"
#include "profile_mutations.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Tabs.H>

#include <algorithm>

namespace layerc::editor {
namespace {
class CommandButton final : public Fl_Button {
public:
  CommandButton(int x, int y, int width, const char *label, EditorApp &app, EditorCommand action)
      : Fl_Button(x, y, width, 30, label), app_(app), action_(action) {
    callback([](Fl_Widget *, void *data) {
      auto &self = *static_cast<CommandButton *>(data);
      self.app_.command(self.action_);
    }, this);
  }
private:
  EditorApp &app_;
  EditorCommand action_;
};
void add_choice(Fl_Choice &choice, const std::string &name) {
  // FLTK menu paths interpret '/', '&' and '\\'. Authored names are text.
  const auto ordinal = choice.size();
  const auto index = choice.add(("__layerc_item_" + std::to_string(ordinal)).c_str());
  const auto label = "[" + std::to_string(index + 1) + "] " + name;
  choice.replace(index, label.c_str());
}
}

void EditorApp::build() {
  window.begin();
  menus = std::make_unique<EditorMenus>(*this);
  content = new Fl_Scroll(0, 0, window.w(), window.h() - EditorMenus::bar_height);
  content->begin();
  auto button = [&](int x, int y, int width, const char *label, EditorCommand action) {
    return new CommandButton(x, y, width, label, *this, action);
  };
  auto input = [&](int x, int y, int width, const char *label) {
    auto *field = new Fl_Input(x, y, width, 28, label);
    field->align(FL_ALIGN_TOP_LEFT); field->when(FL_WHEN_CHANGED);
    field->callback([](Fl_Widget *w, void *data) { static_cast<EditorApp *>(data)->field_changed(w); }, this);
    return field;
  };
  auto choice = [&](int x, int y, int width, const char *label, const char *items) {
    auto *field = new Fl_Choice(x, y, width, 28, label);
    field->align(FL_ALIGN_TOP_LEFT);
    if (items) field->add(items);
    field->value(0);
    field->callback([](Fl_Widget *w, void *data) { static_cast<EditorApp *>(data)->field_changed(w); }, this);
    return field;
  };
  auto check = [&](int x, int y, int width, const char *label) {
    auto *field = new Fl_Check_Button(x, y, width, 28, label);
    field->callback([](Fl_Widget *w, void *data) { static_cast<EditorApp *>(data)->field_changed(w); }, this);
    return field;
  };
  auto browser = [&](int x, int y, int width, int height, const char *label,
                     EditorMenuContext context) {
    auto *field = new ContextBrowser(x, y, width, height, label, *this, context);
    field->align(FL_ALIGN_TOP_LEFT); field->format_char(0);
    field->callback([](Fl_Widget *w, void *data) { static_cast<EditorApp *>(data)->selection_changed(w); }, this);
    return field;
  };
  button(15, 15, 70, "&New", EditorCommand::new_profile)->shortcut(FL_CTRL | 'n');
  button(90, 15, 85, "Duplicate", EditorCommand::duplicate);
  button(180, 15, 75, "Delete", EditorCommand::delete_profile);
  profiles = browser(15, 70, 240, 385, "Profiles", EditorMenuContext::profiles);
  button(15, 465, 115, "Toggle saved", EditorCommand::toggle_enabled);
  button(140, 465, 115, "&Import...", EditorCommand::import_profiles);
  button(15, 505, 115, "Export JSON...", EditorCommand::export_json);
  button(140, 505, 115, "Export pack...", EditorCommand::export_pack);
  module_status = new Fl_Multiline_Output(15, 550, 240, 135, "Preservation / module faults");
  module_status->align(FL_ALIGN_TOP_LEFT);
  module_status->textsize(11);

  tabs = new EditorTabs(270, 15, 775, 435);
  tabs->begin();
  profile_tab = new Fl_Group(270, 45, 775, 405, "Profile");
  profile_tab->begin();
  name = input(285, 80, 340, "Profile name");
  executable = input(660, 80, 370, "Executable name");
  executable_path = input(285, 145, 745, "Executable path (optional)");
  window_class = input(285, 210, 340, "Window class (optional)");
  window_title = input(660, 210, 370, "Window title (optional)");
  enabled = check(285, 265, 260, "Profile enabled");
  profile_id = new Fl_Multiline_Output(285, 320, 745, 30, "Profile ID (read-only)");
  profile_id->align(FL_ALIGN_TOP_LEFT);
  auto *note = new Fl_Box(285, 370, 745, 65,
      "Explicit Save publishes edits and requests an authenticated LayerD reload.\n"
      "Offline editing is supported. The Automation tab edits rules and replays events.\n"
      "The Visual tab calibrates X11 recognition; Game Bar companion data are preserved.");
  note->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
  profile_tab->end();

  cooldown_tab = new Fl_Group(270, 45, 775, 405, "Cooldowns");
  cooldown_tab->begin();
  cooldowns = browser(285, 80, 230, 290, "Cooldowns", EditorMenuContext::cooldowns);
  button(285, 390, 110, "Add cooldown", EditorCommand::add_cooldown);
  button(405, 390, 110, "Remove", EditorCommand::delete_cooldown);
  cooldown_fields = new Fl_Group(535, 65, 495, 325);
  cooldown_fields->begin();
  cooldown_name = input(545, 90, 470, "Cooldown name");
  duration = input(545, 155, 220, "Duration (seconds)");
  charges = input(795, 155, 220, "Max charges");
  recovery = choice(545, 225, 470, "Recovery mode", "Single|Sequential|Parallel");
  retrigger = choice(545, 295, 470, "Retrigger policy", "Ignore|Restart");
  cooldown_fields->end();
  cooldown_tab->end();

  binding_tab = new Fl_Group(270, 45, 775, 405, "Bindings");
  binding_tab->begin();
  bindings = browser(285, 80, 400, 240, "Bindings (unsupported device tokens are retained)",
                      EditorMenuContext::bindings);
  button(285, 350, 160, "Remove selected", EditorCommand::delete_binding);
  binding_target = choice(710, 80, 320, "Target cooldown", nullptr);
  key_code = input(710, 145, 155, "Physical scan code");
  key_code->value("18");
  button(880, 145, 150, "Add key", EditorCommand::add_keyboard);
  mouse_button = choice(710, 210, 155, "Mouse input", "Left|Right|Middle|Back|Forward|Wheel up|Wheel down");
  button(880, 210, 150, "Add mouse", EditorCommand::add_mouse);
  button(710, 275, 320, "Capture focused chord...", EditorCommand::capture_binding);
  auto *capture_note = new Fl_Box(710, 330, 320, 80,
      "Capture reads only events in its focused editor dialog.\n"
      "Controller and HID bindings stay read-only until device capture is implemented.");
  capture_note->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
  capture_note->labelsize(11);
  binding_tab->end();

  panel_tab = new Fl_Group(270, 45, 775, 405, "Panels");
  panel_tab->begin();
  panels = browser(285, 80, 190, 230, "Panels", EditorMenuContext::panels);
  button(285, 325, 90, "Add panel", EditorCommand::add_panel);
  button(385, 325, 90, "Remove", EditorCommand::delete_panel);
  position_button = button(285, 365, 190, "Position on window...", EditorCommand::position);
  panel_fields = new Fl_Group(490, 60, 550, 380);
  panel_fields->begin();
  panel_name = input(500, 85, 260, "Panel name");
  layout = choice(780, 85, 120, "Layout", "Row|Column|Grid");
  style = choice(915, 85, 115, "Style", "Card|Bar");
  anchor_x = input(500, 145, 120, "X (0–1)");
  anchor_y = input(635, 145, 125, "Y (0–1)");
  bar_width = input(780, 145, 120, "Bar width");
  bar_height = input(915, 145, 115, "Bar height");
  fill = input(500, 205, 120, "Fill #RRGGBB");
  background = input(635, 205, 125, "Background");
  border = input(780, 205, 120, "Border");
  text_color = input(915, 205, 115, "Text color");
  show_text = check(500, 255, 180, "Show bar text");
  button(685, 250, 90, "Colours...", EditorCommand::panel_colors);
  hide_ready = check(780, 255, 250, "Hide bars when ready");
  member = choice(500, 320, 260, "Panel cooldown membership", nullptr);
  member_enabled = check(780, 320, 250, "Include selected cooldown");
  auto *preview_note = new Fl_Box(500, 365, 530, 60,
      "Drag panels in the local preview to place them.\n"
      "Ready placeholders remain visible in preview, including hidden-ready bars.");
  preview_note->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
  preview_note->labelsize(11);
  panel_fields->end();
  panel_tab->end();
  automation = std::make_unique<LinuxAutomationEditor>(*this);
  visual = std::make_unique<LinuxVisualEditor>(*this);
  tabs->end();
  active_tab_ = profile_tab;
  tabs->callback([](Fl_Widget *, void *data) { static_cast<EditorApp *>(data)->tab_changed(); }, this);
  preview = new PanelPreview(270, 460, 775, 220, *this);
  button(15, 700, 100, "&Save", EditorCommand::save)->shortcut(FL_CTRL | 's');
  undo_button = button(125, 700, 80, "&Undo", EditorCommand::undo);
  undo_button->shortcut(FL_CTRL | 'z');
  redo_button = button(215, 700, 80, "&Redo", EditorCommand::redo);
  redo_button->shortcut(FL_CTRL | FL_SHIFT | 'z');
  button(305, 700, 130, "Daemon status", EditorCommand::refresh);
  status = new Fl_Multiline_Output(450, 700, 595, 55, "Save / reload outcome");
  status->align(FL_ALIGN_TOP_LEFT);
  daemon_status = new Fl_Multiline_Output(15, 785, 700, 90, "Daemon status and capability faults");
  daemon_status->align(FL_ALIGN_TOP_LEFT);
  daemon_status->textsize(11);
  accessibility_status = new Fl_Box(730, 755, 315, 28);
  accessibility_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  accessibility_status->labelsize(11);
  content->end();
  content->position(0, EditorMenus::bar_height);
  window.end();
  // Keep the authored form geometry intact, with both-axis scrolling on a
  // smaller screen or at HiDPI rather than clipping or overlapping controls.
  window.size_range(640, 480);
  window.resizable(content);
  window.callback([](Fl_Widget *, void *data) {
    auto &app = *static_cast<EditorApp *>(data);
    if (app.may_leave()) app.window.hide();
  }, this);
  accessibility = std::make_unique<EditorAccessibility>(*this);
  accessibility_status->copy_label(accessibility->available()
      ? "Accessibility: AT-SPI bridge active"
      : ("Accessibility unavailable: " + accessibility->fault()).c_str());
  load_appearance();
  if (!session.profiles().profiles.empty()) {
    if (const auto error = session.open(0)) report(*error);
  }
  refresh();
  report(session.has_document() ? "Profile loaded. Explicit Save applies changes." :
         "No readable profiles. Use New or Import to begin. " + session.profiles().error);
  query_status();
  if (!appearance_error_.empty()) report(appearance_error_);
}

void EditorApp::fill_lists() {
  profiles->clear();
  const auto current = session.current_row();
  for (const auto &row : session.profiles().profiles) profiles->add(profile_list_text(row.profile).c_str());
  profiles->value(current ? static_cast<int>(*current + 1) : 0);
  cooldowns->clear(); panels->clear(); bindings->clear();
  if (!session.has_document()) return;
  const auto &profile = session.document().profile();
  if (!cooldown_index()) cooldown_id_ = profile.cooldowns.empty() ? "" : profile.cooldowns.front().id;
  if (!panel_index()) panel_id_ = profile.panels.empty() ? "" : profile.panels.front().id;
  for (const auto &item : profile.cooldowns) cooldowns->add(item.name.c_str());
  for (const auto &item : profile.panels) panels->add(item.name.c_str());
  for (const auto &item : profile.bindings) {
    const auto found = std::ranges::find_if(profile.cooldowns, [&](const auto &c) { return c.id == item.cooldown_id; });
    const auto label = (found == profile.cooldowns.end() ? item.cooldown_id : found->name) +
                       " — " + chord_label(item.chord, profile);
    bindings->add(label.c_str());
  }
  if (const auto index = cooldown_index()) cooldowns->value(static_cast<int>(*index + 1));
  if (const auto index = panel_index()) panels->value(static_cast<int>(*index + 1));
  if (binding_index_ >= static_cast<int>(profile.bindings.size())) binding_index_ = -1;
  bindings->value(binding_index_ + 1);
}

void EditorApp::refresh_forms() {
  if (automation) automation->refresh();
  if (visual) visual->refresh();
  if (!session.has_document()) {
    tabs->deactivate(); profile_id->value("");
    module_status->value("No open profile."); return;
  }
  tabs->activate();
  const auto &profile = session.document().profile();
  profile_id->value(profile.id.c_str());
  name->value(profile.name.c_str());
  executable->value(profile.game.executable_name.c_str());
  executable_path->value(profile.game.executable_path.c_str());
  window_class->value(profile.game.window_class.c_str());
  window_title->value(profile.game.window_title.c_str());
  enabled->value(profile.enabled ? 1 : 0);
  if (const auto index = cooldown_index()) {
    cooldown_fields->activate();
    const auto &item = profile.cooldowns[*index];
    cooldown_name->value(item.name.c_str());
    duration->value(format_seconds(item.duration).c_str());
    charges->value(std::to_string(item.max_charges).c_str());
    recovery->value(static_cast<int>(item.recovery));
    retrigger->value(static_cast<int>(item.retrigger));
  } else cooldown_fields->deactivate();
  const auto target = binding_target->value(), membership = member->value();
  binding_target->clear(); member->clear();
  for (const auto &item : profile.cooldowns) {
    add_choice(*binding_target, item.name);
    add_choice(*member, item.name);
  }
  const auto count = static_cast<int>(profile.cooldowns.size());
  binding_target->value(count ? std::clamp(target, 0, count - 1) : -1);
  member->value(count ? std::clamp(membership, 0, count - 1) : -1);
  member_index_ = member->value();
  if (const auto index = panel_index()) {
    panel_fields->activate();
    const auto &item = profile.panels[*index];
    panel_name->value(item.name.c_str()); layout->value(static_cast<int>(item.layout));
    style->value(static_cast<int>(item.style));
    anchor_x->value(format_decimal(item.anchor_x).c_str()); anchor_y->value(format_decimal(item.anchor_y).c_str());
    bar_width->value(std::to_string(item.bar.width).c_str()); bar_height->value(std::to_string(item.bar.height).c_str());
    fill->value(format_rgb(item.bar.fill_color).c_str()); background->value(format_rgb(item.bar.background_color).c_str());
    border->value(format_rgb(item.bar.border_color).c_str()); text_color->value(format_rgb(item.bar.text_color).c_str());
    show_text->value(item.bar.show_text ? 1 : 0); hide_ready->value(item.bar.hide_when_ready ? 1 : 0);
    const auto selected = member->value();
    member_enabled->value(selected >= 0 && selected < count &&
        std::ranges::find(item.cooldown_ids, profile.cooldowns[static_cast<std::size_t>(selected)].id) != item.cooldown_ids.end());
  } else panel_fields->deactivate();
  std::string modules = "Modules and unsupported bindings are preserved.";
  const auto &document = session.document();
  if (document.has_unreadable_module()) modules += "\nUnreadable module: Save is blocked, not repaired by discarding data.";
  if (!document.visual_error().empty()) modules += "\nVisual: " + document.visual_error();
  if (!session.profiles().warning_text().empty()) modules += "\n" + session.profiles().warning_text();
  module_status->value(modules.c_str());
  module_status->redraw();
}
} // namespace layerc::editor
