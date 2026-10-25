// Deterministic tests of the actual FLTK frontend, not native-input evidence.
// All writes/control attempts are in private fixtures. Native ATK tree
// regressions live in this target; remote AT-SPI behaviour is covered by the
// disposable GUI walkthrough.
#include "editor_app.hpp"
#include "profile_edit.hpp"
#include "automation_graph.hpp"
#include "calibration_window.hpp"
#include "cooldown_groups_window.hpp"
#include "export_selection_window.hpp"
#include "appearance_window.hpp"
#include "linux_editor_test_access.hpp"
#include "test_support.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/calibration_readiness.hpp"
#include "layerc/platform/profile_store.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Window.H>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>

void linux_editor_accessibility_tree_rows();
void linux_editor_diagnostic_text_contracts();
void linux_editor_command_registry_contracts();
void linux_editor_page_lists_clear_without_document();
void linux_editor_semantic_row_extents();
void linux_editor_cooldown_group_dialog_transactions();
void linux_editor_export_profile_checklist();
void linux_editor_system_theme_restores_tab_class_colors();
void linux_editor_field_error_contracts();
void linux_editor_pending_automation_status_contracts();
void linux_editor_calibration_layout_contracts();
void linux_editor_black_accent_dialog_contracts();
void linux_editor_main_window_text_and_tab_labels();
void linux_editor_text_views_fit_content();
void linux_editor_empty_automation_simulator_ready();
void linux_editor_empty_template_apply_is_not_an_edit();

namespace {
namespace editor = layerc::editor;
namespace core = layerc::core;
namespace platform = layerc::platform;
using layerc::test::expect;

class Fixture final {
public:
  Fixture()
      : temp_("layerc-editor-widget"),
        home_("HOME", temp_.path()),
        config_("XDG_CONFIG_HOME", temp_.path() / "config"),
        data_("XDG_DATA_HOME", temp_.path() / "data"),
        runtime_("XDG_RUNTIME_DIR", temp_.path() / "runtime"),
        cache_("XDG_CACHE_HOME", temp_.path() / "cache"),
        state_("XDG_STATE_HOME", temp_.path() / "state"),
        override_("LAYERC_PROFILE_DIR", std::nullopt),
        store_override_("LAYERC_STORE_ROOT", std::nullopt),
        bus_("DBUS_SESSION_BUS_ADDRESS", std::nullopt),
        accessibility_bus_("AT_SPI_BUS_ADDRESS", std::nullopt),
        backend_("FLTK_BACKEND", std::filesystem::path("x11")) {
    layerc::test::require_x11();
    std::filesystem::create_directories(temp_.path() / "runtime");
    expect(::chmod((temp_.path() / "runtime").c_str(), 0700) == 0,
           "fixture control namespace is private");
    path = platform::profile_directory() / "fixture.json";
  }
  void store() {
    auto profile = editor::make_new_profile();
    profile.game.executable_name = "fixture.exe";
    expect(platform::write_profile_file(path, core::serialize_profile_json(profile)).ok(),
           "store only a private fixture");
  }
  std::filesystem::path path;
private:
  layerc::test::TempDirectory temp_;
  layerc::test::ScopedEnvironment home_, config_, data_, runtime_, cache_, state_,
      override_, store_override_, bus_, accessibility_bus_, backend_;
};

struct Reply {
  std::string label;
  int attempts{};
  bool delivered{};
  std::function<void()> before;
  std::string title;
};
Fl_Button *find_button(Fl_Group &group, const std::string &label) {
  for (int i = 0; i < group.children(); ++i) {
    auto *child = group.child(i);
    if (auto *button = dynamic_cast<Fl_Button *>(child);
        button && button->visible_r() && button->label() && label == button->label())
      return button;
    if (auto *nested = dynamic_cast<Fl_Group *>(child))
      if (auto *button = find_button(*nested, label)) return button;
  }
  return nullptr;
}
void reply(void *data) {
  auto &choice = *static_cast<Reply *>(data);
  if (auto *modal = Fl::modal()) {
    if (auto *button = find_button(*modal, choice.label)) {
      if (choice.before) choice.before();
      if (modal->label()) choice.title = modal->label();
      choice.delivered = true; button->do_callback(); return;
    }
    if (++choice.attempts >= 100) { modal->hide(); return; }
  } else if (++choice.attempts >= 100) return;
  Fl::add_timeout(.01, reply, data);
}
class Answer final {
public:
  explicit Answer(std::string label, std::function<void()> before = {})
      : choice_{std::move(label), 0, false, std::move(before), {}} {
    Fl::add_timeout(.01, reply, &choice_);
  }
  ~Answer() { Fl::remove_timeout(reply, &choice_); }
  [[nodiscard]] bool delivered() const { return choice_.delivered; }
  // Title of the modal that received the answer; confirmations name their action.
  [[nodiscard]] const std::string &title() const { return choice_.title; }
private:
  Reply choice_;
};
void show(editor::EditorApp &app) { app.build(); editor::EditorAppTestAccess::window(app).show(); Fl::check(); }
void tab(editor::EditorApp &app, Fl_Group *group) {
  editor::EditorAppTestAccess::tabs(app)->value(group); app.tab_changed();
}

void empty_draft_save_and_reopen() {
  Fixture fixture;
  editor::EditorApp app;
  show(app);
  expect(!app.session.has_document() && !editor::EditorAppTestAccess::tabs(app)->active(),
         "an empty store has no phantom open document");
  app.command(editor::EditorCommand::new_profile);
  expect(app.session.is_draft() && editor::EditorAppTestAccess::tabs(app)->active(), "New enables the draft controls");
  editor::EditorAppTestAccess::name(app)->value("Widget draft"); app.field_changed(editor::EditorAppTestAccess::name(app));
  expect(!app.save() && !std::filesystem::exists(fixture.path),
         "an unmatched draft fails visibly without writing");
  editor::EditorAppTestAccess::executable(app)->value("widget.exe"); app.field_changed(editor::EditorAppTestAccess::executable(app));
  expect(app.save() && !app.session.has_unsaved_changes(),
         "explicit Save commits the accepted draft");
  const auto path = app.session.document().path();
  expect(std::string(editor::EditorAppTestAccess::status(app)->value()).find("LayerD is not running") != std::string::npos,
         "the durable Save separately reports the private daemon absent");
  expect(!app.session.open_path(path) && app.session.document().profile().name == "Widget draft",
         "the saved draft reopens with the accepted fields");
  editor::EditorAppTestAccess::window(app).hide();
}
void text_history_and_invalid_navigation() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  const auto original = app.session.document().profile().name;
  expect(editor::EditorAppTestAccess::profile_id(app)->readonly() &&
         std::string(editor::EditorAppTestAccess::profile_id(app)->value()) == app.session.document().profile().id,
         "read-only profile identity identifies even same-name imports");
  editor::EditorAppTestAccess::name(app)->value("First"); app.field_changed(editor::EditorAppTestAccess::name(app));
  editor::EditorAppTestAccess::name(app)->value("First edited"); app.field_changed(editor::EditorAppTestAccess::name(app));
  app.command(editor::EditorCommand::undo);
  expect(app.session.document().profile().name == original, "one Undo restores coalesced typing");
  app.command(editor::EditorCommand::redo);
  expect(app.session.document().profile().name == "First edited", "Redo restores accepted typing");
  tab(app, editor::EditorAppTestAccess::cooldown_tab(app));
  const auto duration = app.session.document().profile().cooldowns.front().duration;
  editor::EditorAppTestAccess::duration(app)->value("nan"); app.field_changed(editor::EditorAppTestAccess::duration(app));
  expect(std::string(editor::EditorAppTestAccess::status(app)->value()).find("Invalid input") != std::string::npos &&
         app.session.document().profile().cooldowns.front().duration == duration,
         "invalid numeric text stays visible without modifying accepted data");
  tab(app, editor::EditorAppTestAccess::panel_tab(app));
  expect(editor::EditorAppTestAccess::tabs(app)->value() == editor::EditorAppTestAccess::cooldown_tab(app) && std::string(editor::EditorAppTestAccess::duration(app)->value()) == "nan",
         "navigation cannot silently discard an invalid field buffer");
  app.command(editor::EditorCommand::undo);
  expect(app.session.document().profile().cooldowns.front().duration == duration &&
         std::string(editor::EditorAppTestAccess::duration(app)->value()) != "nan",
         "Undo explicitly discards the locally invalid field");
  editor::EditorAppTestAccess::window(app).hide();
}
void cooldowns_bindings_and_panels() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  tab(app, editor::EditorAppTestAccess::cooldown_tab(app));
  const auto count = app.session.document().profile().cooldowns.size();
  app.command(editor::EditorCommand::add_cooldown);
  expect(app.session.document().profile().cooldowns.size() == count + 1,
         "Add cooldown edits the accepted document");
  editor::EditorAppTestAccess::cooldown_name(app)->value("A/B & Unicode Δ"); app.field_changed(editor::EditorAppTestAccess::cooldown_name(app));
  editor::EditorAppTestAccess::charges(app)->value("2"); app.field_changed(editor::EditorAppTestAccess::charges(app));
  editor::EditorAppTestAccess::recovery(app)->value(1); app.field_changed(editor::EditorAppTestAccess::recovery(app));
  expect(app.session.document().profile().cooldowns.back().name == "A/B & Unicode Δ" &&
         app.session.document().profile().cooldowns.back().max_charges == 2,
         "cooldown text and charge/recovery fields are applied");
  tab(app, editor::EditorAppTestAccess::binding_tab(app));
  expect(editor::EditorAppTestAccess::binding_target(app)->size() == static_cast<int>(count + 2),
         "authored slash/ampersand names remain one choice per cooldown");
  editor::EditorAppTestAccess::binding_target(app)->value(static_cast<int>(count));
  const auto binding_count = app.session.document().profile().bindings.size();
  editor::EditorAppTestAccess::key_code(app)->value("18"); app.command(editor::EditorCommand::add_keyboard);
  editor::EditorAppTestAccess::mouse_button(app)->value(5); app.command(editor::EditorCommand::add_mouse);
  const auto &bindings = app.session.document().profile().bindings;
  expect(bindings.size() == binding_count + 2 &&
         bindings.back().chord.tokens.front().code == 6,
         "keyboard and wheel choices create their real profile tokens");
  tab(app, editor::EditorAppTestAccess::panel_tab(app));
  const auto panel_count = app.session.document().profile().panels.size();
  app.command(editor::EditorCommand::add_panel);
  editor::EditorAppTestAccess::anchor_x(app)->value("0.25"); app.field_changed(editor::EditorAppTestAccess::anchor_x(app));
  editor::EditorAppTestAccess::fill(app)->value("#123456"); app.field_changed(editor::EditorAppTestAccess::fill(app));
  expect(app.session.document().profile().panels.size() == panel_count + 1 &&
         app.session.document().profile().panels.back().anchor_x == .25 &&
         app.session.document().profile().panels.back().bar.fill_color == 0x123456,
         "panel creation, local placement and color controls use accepted edits");
  editor::EditorAppTestAccess::fill(app)->value("not-a-color"); app.field_changed(editor::EditorAppTestAccess::fill(app));
  expect(!app.save() && std::string(editor::EditorAppTestAccess::fill(app)->value()) == "not-a-color",
         "Save refuses an invalid panel color and retains the field buffer");
  editor::EditorAppTestAccess::window(app).hide();
}
void dirty_cancel_discard_and_delete() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  editor::EditorAppTestAccess::name(app)->value("Unsaved"); app.field_changed(editor::EditorAppTestAccess::name(app));
  // A valid pending edit now autosaves on leave. Invalid field buffers still
  // require Cancel/Discard/Save and may never publish the accepted draft.
  editor::EditorAppTestAccess::name(app)->value(""); app.field_changed(editor::EditorAppTestAccess::name(app));
  const auto before = layerc::test::read_file(fixture.path);
  {
    Answer answer("Cancel");
    expect(!app.may_leave(), "Cancel blocks leaving the dirty profile");
    expect(answer.delivered() && answer.title() == "Unsaved changes",
           "Cancel was delivered to the titled unsaved-changes dialog");
  }
  expect(app.session.document().profile().name == "Unsaved" &&
         layerc::test::read_file(fixture.path) == before, "cancel keeps accepted edits and the disk unchanged");
  {
    Answer answer("Discard");
    expect(app.may_leave(), "Discard reloads the saved document");
    expect(answer.delivered() && answer.title() == "Unsaved changes",
           "Discard was delivered to the titled unsaved-changes dialog");
  }
  expect(!app.session.has_unsaved_changes() && layerc::test::read_file(fixture.path) == before,
         "discarding an edit never writes the store");
  {
    Answer answer("Cancel");
    app.command(editor::EditorCommand::delete_profile);
    expect(answer.delivered() && answer.title() == "Delete profile" &&
               std::filesystem::exists(fixture.path),
           "canceled deletion leaves the profile on disk");
  }
  {
    Answer answer("Delete");
    app.command(editor::EditorCommand::delete_profile);
    expect(answer.delivered() && answer.title() == "Delete profile" &&
               !std::filesystem::exists(fixture.path) &&
           !app.session.has_document(), "confirmed deletion uses the real profile transaction");
  }
  editor::EditorAppTestAccess::window(app).hide();
}
void capture_cancel_and_small_window() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  tab(app, editor::EditorAppTestAccess::binding_tab(app));
  const auto before = app.session.document().profile();
  {
    Answer answer("Cancel");
    app.command(editor::EditorCommand::capture_binding);
    expect(answer.delivered(), "focused capture opens its owned cancelable dialog");
  }
  expect(app.session.document().profile() == before, "canceled capture changes no binding");
  editor::EditorAppTestAccess::window(app).size(640, 480); Fl::check();
  expect(editor::EditorAppTestAccess::content(app)->w() == 640 && editor::EditorAppTestAccess::content(app)->h() == 480 - editor::EditorMenus::bar_height &&
         editor::EditorAppTestAccess::status(app)->w() == 595 && editor::EditorAppTestAccess::panel_name(app)->w() == 260,
         "a small viewport scrolls intact forms rather than squeezing them into overlap");
  editor::EditorAppTestAccess::content(app)->scroll_to(0, 300); Fl::check();
  expect(editor::EditorAppTestAccess::content(app)->yposition() > 0, "the small viewport exposes the bottom status area by scrolling");
  editor::EditorAppTestAccess::window(app).hide();
}
const Fl_Menu_Item &menu_item(Fl_Menu_ &menu, const char *path) {
  const auto *item = menu.find_item(path);
  expect(item != nullptr, "the actual menu contains the command");
  return *item;
}
void activate_menu(Fl_Menu_ &menu, const char *path) {
  const auto item = menu_item(menu, path);
  item.do_callback(&menu);
}
void menus_follow_document_history_and_context() {
  Fixture fixture;
  editor::EditorApp app; show(app);
  auto &bar = editor::EditorAppTestAccess::menus(app)->bar();
  expect(menu_item(bar, "&File/&New profile").active() &&
         !menu_item(bar, "&File/&Save").active() &&
         !menu_item(bar, "&Profile/&Delete...").active() &&
         !menu_item(bar, "&Edit/&Undo").active(),
         "an empty store enables New but not document/history commands");
  activate_menu(bar, "&File/&Save");
  expect(!app.session.has_document(), "even a direct disabled callback cannot create a document");
  activate_menu(bar, "&File/&New profile");
  expect(app.session.is_draft() && !menu_item(bar, "&Profile/&Delete...").active(),
         "New routes through the session model and draft deletion remains disabled");
  editor::EditorAppTestAccess::executable(app)->value("menu.exe"); app.field_changed(editor::EditorAppTestAccess::executable(app));
  activate_menu(bar, "&File/&Save");
  expect(!app.session.is_draft() && !app.session.has_unsaved_changes() &&
         menu_item(bar, "&Profile/&Delete...").active(), "menu Save durably publishes the draft");
  const auto original = app.session.document().profile().name;
  editor::EditorAppTestAccess::name(app)->value("Menu edit"); app.field_changed(editor::EditorAppTestAccess::name(app));
  expect(menu_item(bar, "&Edit/&Undo").active(), "accepted text updates menu history state");
  activate_menu(bar, "&Edit/&Undo");
  expect(app.session.document().profile().name == original &&
         menu_item(bar, "&Edit/&Redo").active(), "menu Undo restores data and enables Redo");
  activate_menu(bar, "&Edit/&Redo");
  expect(app.session.document().profile().name == "Menu edit", "menu Redo restores the accepted edit");
  Fl_Menu_Button context(0, 0, 0, 0);
  const auto before = app.session.document().profile();
  editor::EditorAppTestAccess::menus(app)->configure_context(context, editor::EditorMenuContext::profiles, false);
  expect(!menu_item(context, "&Duplicate").active() &&
         menu_item(context, "&New profile").active(), "empty-area context does not inherit selected actions");
  activate_menu(context, "&Duplicate");
  expect(app.session.document().profile() == before && !app.session.is_draft(),
         "a disabled empty-area callback cannot duplicate the previous selection");
  editor::EditorAppTestAccess::menus(app)->configure_context(context, editor::EditorMenuContext::profiles, true);
  expect(menu_item(context, "&Duplicate").active(), "selected-row context enables its own commands");
  {
    Answer answer("Cancel");
    activate_menu(context, "&Delete...");
    expect(answer.delivered() && std::filesystem::exists(app.session.document().path()),
           "menu deletion obeys dirty-cancel and preserves the store");
  }
  activate_menu(bar, "&View/&Cooldowns");
  editor::EditorAppTestAccess::duration(app)->value("nan"); app.field_changed(editor::EditorAppTestAccess::duration(app));
  activate_menu(bar, "&View/&Panels");
  expect(editor::EditorAppTestAccess::tabs(app)->value() == editor::EditorAppTestAccess::cooldown_tab(app) &&
         std::string(editor::EditorAppTestAccess::duration(app)->value()) == "nan", "menu navigation retains an invalid local field buffer");
  activate_menu(bar, "&Edit/&Undo");
  expect(std::string(editor::EditorAppTestAccess::duration(app)->value()) != "nan", "menu Undo explicitly discards invalid text");
  editor::EditorAppTestAccess::window(app).hide();
}
void context_rows_and_fixed_menu_geometry() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  auto *list = dynamic_cast<editor::ContextBrowser *>(editor::EditorAppTestAccess::profiles(app));
  expect(list != nullptr &&
         list->row_at(list->x() + 10, list->y() + 10) == 1 &&
         list->row_at(list->x() + 10, list->y() + list->h() - 10) == 0,
         "FLTK row bounds distinguish a real row from empty list space");
  const auto row = list->row_bounds(1);
  expect(row && row->height > 0 && row->height < list->h() && row->contains(list->x() + 10, list->y() + 10) &&
         !list->row_bounds(list->size() + 1),
         "accessible row extents are one visible row, not the whole browser");
  auto *tabs = dynamic_cast<editor::EditorTabs *>(editor::EditorAppTestAccess::tabs(app));
  const auto first = tabs ? tabs->tab_bounds(0) : std::nullopt;
  const auto second = tabs ? tabs->tab_bounds(1) : std::nullopt;
  expect(first && second && first->x + first->width <= second->x + 1 && first->height < 40 &&
         tabs->which(first->x + first->width / 2, first->y + first->height / 2) == editor::EditorAppTestAccess::profile_tab(app) &&
         tabs->which(second->x + second->width / 2, second->y + second->height / 2) == editor::EditorAppTestAccess::cooldown_tab(app) &&
         !tabs->tab_bounds(tabs->children()),
         "accessible tab extents match FLTK's own tab hit testing");
  auto &bar = editor::EditorAppTestAccess::menus(app)->bar();
  editor::EditorAppTestAccess::window(app).size(640, 480); Fl::check();
  const int bar_y = bar.y();
  editor::EditorAppTestAccess::content(app)->scroll_to(0, 200); Fl::check();
  expect(bar_y == 0 && bar.y() == 0 && bar.w() == 640 &&
         editor::EditorAppTestAccess::content(app)->y() == editor::EditorMenus::bar_height &&
         editor::EditorAppTestAccess::content(app)->h() == 480 - editor::EditorMenus::bar_height,
         "the top-level menu remains fixed above the small scrolling viewport");
  Fl_Menu_Button context(0, 0, 0, 0);
  editor::EditorAppTestAccess::menus(app)->configure_context(context, editor::EditorMenuContext::cooldowns, false);
  expect(menu_item(context, "&Add cooldown").active() &&
         !menu_item(context, "&Remove cooldown...").active(),
         "empty cooldown context permits Add but not removal of a previous row");
  editor::EditorAppTestAccess::menus(app)->configure_context(context, editor::EditorMenuContext::cooldowns, true);
  const auto before = app.session.document().profile().cooldowns.size();
  activate_menu(context, "&Add cooldown");
  expect(app.session.document().profile().cooldowns.size() == before + 1,
         "context Add uses the same validated command as the button");
  editor::EditorAppTestAccess::window(app).hide();
}
void document_shortcuts_and_native_text_keys() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  const auto original = app.session.document().profile().name;
  Fl::focus(editor::EditorAppTestAccess::name(app));
  editor::EditorAppTestAccess::name(app)->value("Shortcut edit"); app.field_changed(editor::EditorAppTestAccess::name(app));
  expect(editor::EditorAppTestAccess::menus(app)->shortcut('z', FL_CTRL) &&
         app.session.document().profile().name == original,
         "document Ctrl+Z uses accepted history with a text field focused");
  expect(editor::EditorAppTestAccess::menus(app)->shortcut('Z', FL_CTRL | FL_SHIFT) &&
         app.session.document().profile().name == "Shortcut edit",
         "Ctrl+Shift+Z restores the accepted document, not a widget-only buffer");
  expect(editor::EditorAppTestAccess::menus(app)->shortcut('z', FL_CTRL) && editor::EditorAppTestAccess::menus(app)->shortcut('y', FL_CTRL) &&
         app.session.document().profile().name == "Shortcut edit",
         "Ctrl+Y redoes like the Windows editor");
  expect(!editor::EditorAppTestAccess::menus(app)->shortcut('a', FL_CTRL) && !editor::EditorAppTestAccess::menus(app)->shortcut('c', FL_CTRL) &&
         !editor::EditorAppTestAccess::menus(app)->shortcut('v', FL_CTRL) && !editor::EditorAppTestAccess::menus(app)->shortcut('e', FL_CTRL) &&
         !editor::EditorAppTestAccess::menus(app)->shortcut('z', FL_CTRL | FL_ALT),
         "native select/copy/paste/end and unmatched modifier combinations are not intercepted");
  expect(editor::EditorAppTestAccess::menus(app)->shortcut('s', FL_CTRL) && !app.session.has_unsaved_changes() &&
         layerc::test::read_file(fixture.path).value_or("").find("Shortcut edit") != std::string::npos,
         "the shared shortcut dispatcher durably saves accepted edits");
  editor::EditorAppTestAccess::window(app).hide();
}

// A real key event reaches the focused input before its window; document
// undo must still win over the field's own local undo buffer.
void routed_ctrl_z_precedes_field_undo() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  const auto original = app.session.document().profile().game.executable_name;
  Fl::focus(editor::EditorAppTestAccess::executable(app));
  editor::EditorAppTestAccess::executable(app)->insert_position(editor::EditorAppTestAccess::executable(app)->size());
  (void)editor::EditorAppTestAccess::executable(app)->replace(editor::EditorAppTestAccess::executable(app)->size(), editor::EditorAppTestAccess::executable(app)->size(), "x", 1);
  app.field_changed(editor::EditorAppTestAccess::executable(app));
  expect(app.apply_fields() && app.session.document().profile().game.executable_name == original + "x",
         "the typed executable name is an accepted document edit");
  Fl::e_keysym = 'z';
  Fl::e_state = FL_CTRL;
  Fl::e_text = const_cast<char *>("\x1a");
  Fl::e_length = 1;
  expect(Fl::handle(FL_KEYBOARD, &editor::EditorAppTestAccess::window(app)) != 0 &&
         app.session.document().profile().game.executable_name == original,
         "a dispatched Ctrl+Z undoes the document edit, not only the field's text");
  Fl::e_state = 0;
  editor::EditorAppTestAccess::window(app).hide();
}
Fl_Widget *find_control(Fl_Group &group, const std::string_view label) {
  for (int i = 0; i < group.children(); ++i) {
    auto *child = group.child(i);
    if (child->label() && label == child->label()) return child;
    if (auto *nested = dynamic_cast<Fl_Group *>(child))
      if (auto *found = find_control(*nested, label)) return found;
  }
  return nullptr;
}
template <typename Widget>
Widget &automation_control(editor::EditorApp &app, const std::string_view label) {
  auto *widget = dynamic_cast<Widget *>(find_control(*editor::EditorAppTestAccess::automation(app)->group(), label));
  expect(widget != nullptr, "the automation page owns the named control");
  return *widget;
}
void automation_history_and_invalid_properties() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  app.command(editor::EditorCommand::automation_page);
  expect(editor::EditorAppTestAccess::tabs(app)->value() == editor::EditorAppTestAccess::automation(app)->group(), "View Automation opens the real page");
  auto &create = automation_control<Fl_Button>(app, "New automation");
  create.do_callback();
  expect(app.session.document().automation() && app.session.can_undo(),
         "a blank preset goes through document history");
  auto &palette = automation_control<Fl_Choice>(app, "Node palette");
  palette.value(0);
  automation_control<Fl_Button>(app, "Add node").do_callback();
  expect(app.session.document().automation()->automations.front().rules.size() == 1,
         "the node palette adds a validated input rule");
  const auto graph_before = editor::automation_graph_json(app.session.document().automation());
  auto &priority = automation_control<Fl_Input>(app, "Priority");
  priority.value("bad"); priority.do_callback();
  automation_control<Fl_Button>(app, "Apply properties").do_callback();
  expect(editor::automation_graph_json(app.session.document().automation()) == graph_before &&
         std::string(priority.value()) == "bad" && !app.save(),
         "invalid properties stay local and Save cannot publish them");
  tab(app, editor::EditorAppTestAccess::profile_tab(app));
  expect(editor::EditorAppTestAccess::tabs(app)->value() == editor::EditorAppTestAccess::automation(app)->group() && std::string(priority.value()) == "bad",
         "invalid automation fields block tab navigation without discarding text");
  app.command(editor::EditorCommand::undo);
  expect(editor::automation_graph_json(app.session.document().automation()) == graph_before && std::string(priority.value()) != "bad",
         "Undo explicitly discards the invalid property buffer without undoing the graph");
  app.command(editor::EditorCommand::undo);
  expect(app.session.document().automation()->automations.front().rules.empty(),
         "the next Undo reverses the accepted palette edit");
  app.command(editor::EditorCommand::redo);
  expect(editor::automation_graph_json(app.session.document().automation()) == graph_before, "Redo restores the accepted graph");
  auto &name = automation_control<Fl_Input>(app, "Automation name");
  name.value("A/B & Unicode Δ"); name.do_callback();
  automation_control<Fl_Button>(app, "Apply properties").do_callback();
  expect(app.session.document().automation()->automations.front().name == "A/B & Unicode Δ",
         "Apply properties accepts UTF-8 names without menu-label parsing");
  expect(app.save() && !app.session.has_unsaved_changes(), "Save durably publishes the automation sidecar");
  const auto saved = editor::automation_graph_json(app.session.document().automation());
  expect(!app.session.open_path(fixture.path) && editor::automation_graph_json(app.session.document().automation()) == saved,
         "the saved graph reopens byte-stably through the durable store");
  editor::EditorAppTestAccess::window(app).hide();
}
void automation_json_and_simulator_lifetime() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  app.command(editor::EditorCommand::automation_page);
  automation_control<Fl_Button>(app, "New automation").do_callback();
  const auto accepted = editor::automation_graph_json(app.session.document().automation());
  const auto disk_before = layerc::test::read_file(fixture.path);
  auto &view = automation_control<Fl_Choice>(app, "Automation view");
  view.value(2); view.do_callback();
  auto &json = automation_control<Fl_Multiline_Input>(app, "Advanced automation JSON");
  expect(std::string_view(json.value()).find('\r') == std::string_view::npos &&
             std::string_view(json.value()).find('\n') != std::string_view::npos,
         "Advanced JSON shows plain lines, never CR glyphs");
  json.value("{invalid"); json.do_callback();
  automation_control<Fl_Button>(app, "Validate / Apply JSON").do_callback();
  expect(editor::automation_graph_json(app.session.document().automation()) == accepted && !app.save() &&
         layerc::test::read_file(fixture.path) == disk_before &&
         std::string(editor::EditorAppTestAccess::status(app)->value()).find("Invalid automation JSON") != std::string::npos,
         "JSON parser refusal preserves the accepted graph and every durable byte");
  view.value(1); view.do_callback();
  expect(view.value() == 2 && std::string(json.value()) == "{invalid",
         "a failed view change preserves invalid JSON");
  app.command(editor::EditorCommand::undo);
  auto replacement = app.session.document().automation();
  replacement->automations.front().name = "Applied JSON";
  const auto text = editor::automation_graph_json(replacement);
  json.value(text.c_str()); json.do_callback();
  automation_control<Fl_Button>(app, "Validate / Apply JSON").do_callback();
  expect(editor::automation_graph_json(app.session.document().automation()) == text,
         "validated advanced JSON uses the same document transaction as Basic");
  view.value(1); view.do_callback();
  auto &state = automation_control<Fl_Multiline_Output>(app, "Simulator state");
  expect(std::string(state.value()).find("Events: 0/1") == 0,
         "the simulator seeds sessionStarted and does not run the real daemon");
  automation_control<Fl_Button>(app, "Step").do_callback();
  expect(std::string(state.value()).find("Events: 1/1") == 0, "Step consumes one deterministic root");
  auto &fields = automation_control<Fl_Input>(app, "Fields: name=JSON; ...");
  fields.value("nonsense");
  automation_control<Fl_Button>(app, "Add event").do_callback();
  expect(std::string(state.value()).find("Events: 1/1") == 0,
         "invalid synthetic fields cannot append a partial event");
  fields.value("");
  automation_control<Fl_Button>(app, "Add event").do_callback();
  expect(std::string(state.value()).find("Events: 1/2") == 0, "a valid event appends at virtual time");
  auto &play = automation_control<Fl_Button>(app, "Play");
  play.do_callback();
  expect(std::string(play.label()) == "Pause", "Play owns its scheduled timeout");
  view.value(0); view.do_callback();
  expect(std::string(play.label()) == "Play", "leaving the simulator cancels playback immediately");
  const auto before_wait = std::string(state.value());
  Fl::wait(.05);
  expect(std::string(state.value()) == before_wait,
         "a hidden simulator cannot keep advancing on an orphaned timeout");
  view.value(1); view.do_callback();
  automation_control<Fl_Choice>(app, "Speed").value(2);
  play.do_callback();
  for (int i = 0; i < 30 && std::string(play.label()) == "Pause"; ++i) Fl::wait(.02);
  expect(std::string(play.label()) == "Play" && std::string(state.value()).find("Events: 2/2") == 0,
         "playback consumes the remaining event and becomes idle without polling");
  automation_control<Fl_Button>(app, "Reset").do_callback();
  play.do_callback();
  editor::EditorAppTestAccess::window(app).hide(); // destruction removes the owned timeout before FLTK frees its widgets
}
void automation_trace_disconnect() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  app.command(editor::EditorCommand::automation_page);
  auto &view = automation_control<Fl_Choice>(app, "Automation view");
  view.value(3); view.do_callback();
  auto &record = automation_control<Fl_Check_Button>(
      app, "Record authenticated LayerD trace (saved profile only)");
  auto &status = automation_control<Fl_Multiline_Output>(app, "Live trace status");
  const auto before = editor::automation_graph_json(app.session.document().automation());
  const auto disk = layerc::test::read_file(fixture.path);
  record.value(1); record.do_callback();
  for (int i = 0; i < 80 && record.value(); ++i) Fl::wait(.02);
  expect(!record.value() && std::string(status.value()).find("Unavailable:") == 0 &&
             std::string(editor::EditorAppTestAccess::status(app)->value()).find("Automation trace unavailable") == 0,
         "missing authenticated daemon is visible and stops the trace worker");
  expect(editor::automation_graph_json(app.session.document().automation()) == before &&
             layerc::test::read_file(fixture.path) == disk && !app.session.has_unsaved_changes(),
         "trace failure changes neither the document nor its durable profile");
  record.value(1); record.do_callback();
  view.value(0); view.do_callback();
  expect(!record.value() && std::string(status.value()).find("Stopped.") == 0,
         "leaving trace view joins its owned worker and cancels the UI timer");
  Fl::wait(.05);
  expect(std::string(status.value()).find("Stopped.") == 0, "no orphaned callback changes hidden status");
  editor::EditorAppTestAccess::window(app).hide();
}

template <typename Widget>
Widget &visual_control(editor::EditorApp &app, const std::string_view label) {
  auto *widget = dynamic_cast<Widget *>(find_control(*editor::EditorAppTestAccess::visual(app)->group(), label));
  expect(widget != nullptr, "the visual page owns the named control");
  return *widget;
}
void visual_page_history_and_cancel() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  expect(app.session.has_document(), "show opens the only private fixture");
  expect(editor::EditorAppTestAccess::visual(app)->group()->children() == 0, "hidden Visual controls are demand-created");
  app.command(editor::EditorCommand::visual_page);
  expect(editor::EditorAppTestAccess::tabs(app)->value() == editor::EditorAppTestAccess::visual(app)->group(), "View Visual opens the real page");
  const auto profile = app.session.document().profile();
  const auto id = profile.cooldowns.front().id;
  auto seed = editor::CalibrationModel::open(profile, id, std::nullopt);
  expect(seed.has_value(), "visual seed uses the selected cooldown");
  auto built = seed->build_result(seed->initial_controls(), {});
  expect(std::holds_alternative<core::VisualProfile>(built), "pixel calibration produces a valid module");
  auto prepared = app.session.prepare_calibration(id, std::get<core::VisualProfile>(built), {},
      [] { return editor::SharedObservationChoice::update_shared; });
  auto *accepted = std::get_if<editor::AcceptedCalibration>(&prepared);
  expect(accepted != nullptr && !app.session.apply_calibration(std::move(*accepted)),
         "apply the validated visual draft");
  app.refresh();
  auto &enabled = visual_control<Fl_Check_Button>(app, "Visual trigger enabled");
  enabled.value(0); enabled.do_callback();
  expect(!editor::find_visual_trigger(*app.session.document().visual(), id)->enabled,
         "Enabled updates the complete visual draft");
  app.command(editor::EditorCommand::undo);
  expect(editor::find_visual_trigger(*app.session.document().visual(), id)->enabled,
         "document Undo restores the trigger");
  {
    Answer remove("Remove assignment");
    visual_control<Fl_Button>(app, "Remove visual assignment").do_callback();
    expect(remove.delivered() && remove.title() == "Remove visual assignment",
           "Remove has an explicit titled confirmation");
  }
  expect(!app.session.document().visual(), "Remove erases the unreferenced observation and trigger");
  app.command(editor::EditorCommand::undo);
  expect(app.session.document().visual().has_value(), "Undo restores the removed visual module");
  const auto before = *app.session.document().visual();
  {
    Answer cancel("Cancel calibration");
    visual_control<Fl_Button>(app, "Calibrate visual...").do_callback();
    expect(cancel.delivered(), "calibration Cancel belongs to the real modal");
  }
  expect(*app.session.document().visual() == before &&
         std::string(editor::EditorAppTestAccess::status(app)->value()).find("Calibration cancelled") == 0,
         "Cancel preserves the complete unsaved visual draft and explains no writes");
  auto &isolated = visual_control<Fl_Check_Button>(app, "Isolated visual runtime");
  isolated.value(1); isolated.do_callback();
  expect(app.session.document().profile().visual_runtime == core::VisualRuntime::isolated &&
         std::string(visual_control<Fl_Multiline_Output>(app,
             "Visual availability / preservation").value()).find("Unavailable:") != std::string::npos,
         "unsupported isolated runtime is retained with an explicit reason");
  app.command(editor::EditorCommand::undo);
  expect(app.session.document().profile().visual_runtime == core::VisualRuntime::inline_process,
         "runtime setting participates in document history");
  editor::EditorAppTestAccess::window(app).hide();
}
void visual_remove_refuses_nested_selection_change() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  app.command(editor::EditorCommand::add_cooldown);
  app.command(editor::EditorCommand::visual_page);
  const auto profile = app.session.document().profile();
  for (const auto &cooldown : profile.cooldowns) {
    auto seed = editor::CalibrationModel::open(profile, cooldown.id, app.session.document().visual());
    auto built = seed->build_result(seed->initial_controls(), {});
    expect(std::holds_alternative<core::VisualProfile>(built), "seed each private visual assignment");
    auto prepared = app.session.prepare_calibration(cooldown.id,
        std::get<core::VisualProfile>(built), {},
        [] { return editor::SharedObservationChoice::update_shared; });
    auto *accepted = std::get_if<editor::AcceptedCalibration>(&prepared);
    expect(accepted != nullptr && !app.session.apply_calibration(std::move(*accepted)),
           "apply each private assignment");
  }
  app.refresh();
  auto &target = visual_control<Fl_Choice>(app, "Visual target cooldown");
  target.value(0); target.do_callback();
  const auto before = app.session.document().visual();
  {
    Answer answer("Remove assignment", [&] { target.value(1); target.do_callback(); });
    visual_control<Fl_Button>(app, "Remove visual assignment").do_callback();
    expect(answer.delivered(), "a nested event changes selection inside the real Remove confirmation");
  }
  expect(app.session.document().visual() == before &&
         std::string(editor::EditorAppTestAccess::status(app)->value()).find("Not removed:") == 0,
         "confirmation for A cannot remove B after a nested selection change");
  {
    Answer answer("Remove assignment", [&] {
      expect(!app.session.edit([](auto &candidate) { candidate.name = "Nested document edit"; }),
             "a nested document mutation succeeds independently");
    });
    visual_control<Fl_Button>(app, "Remove visual assignment").do_callback();
    expect(answer.delivered(), "a nested event changes the document inside Remove confirmation");
  }
  expect(app.session.document().visual() == before &&
         app.session.document().profile().name == "Nested document edit" &&
         std::string(editor::EditorAppTestAccess::status(app)->value()).find("Not removed:") == 0,
         "changed document refuses stale removal without discarding the nested edit");
  editor::EditorAppTestAccess::window(app).hide();
}
struct CalibrationProbe {
  bool delivered{};
  bool capture{};
  bool refuse_once{};
  int attempts{};
};
void exercise_calibration(void *data) {
  auto &probe = *static_cast<CalibrationProbe *>(data);
  auto *modal = Fl::modal();
  if (!modal) {
    if (++probe.attempts < 100) Fl::add_timeout(.01, exercise_calibration, data);
    return;
  }
  const auto control = [&](const char *label) {
    auto *widget = find_control(*modal, label);
    expect(widget != nullptr, "real calibration owns each named control");
    return widget;
  };
  auto &live = *dynamic_cast<Fl_Check_Button *>(control("Live preview"));
  auto &status = *dynamic_cast<Fl_Multiline_Output *>(control("Capture / live match status"));
  auto *apply = control("Apply calibration");
  expect(!apply->active(), "untouched calibration must disable Apply before any captured frame");
  apply->do_callback();
  expect(Fl::modal() == modal &&
             std::string(status.value()) == core::calibration_sampling_required,
         "programmatic Apply cannot bypass unsampled refusal or dismiss the draft");
  live.value(1); live.do_callback();
  expect(!live.value() && std::string(status.value()).find("Unavailable:") == 0,
         "capture without acknowledgement is refused visibly");
  if (!probe.capture) {
    control("Rescan windows")->do_callback();
    expect(std::string(status.value()).find("Unavailable:") == 0,
           "missing target has a visible unavailable reason");
    control("Cancel calibration")->do_callback(); probe.delivered = true; return;
  }
  control("Rescan windows")->do_callback();
  auto &target = *dynamic_cast<Fl_Choice *>(control("Calibration target window"));
  expect(target.value() >= 0, "rescan finds the private matching X11 fixture");
  auto &notice = *dynamic_cast<Fl_Check_Button *>(control(
      "I acknowledge passive capture of the selected game window"));
  notice.value(1); notice.do_callback();
  live.value(1); live.do_callback();
  auto &preview = *dynamic_cast<editor::CalibrationPreview *>(control("Calibration preview"));
  expect(live.value() && preview.frame && preview.frame->view().valid(),
         "acknowledged real X11 capture produces a valid owned frame");
  control("Capture reference")->do_callback();
  expect(std::string(dynamic_cast<Fl_Output *>(control("Template references"))->value()).find("1/") == 0,
         "capture stages one structured template reference");
  expect(apply->active(),
         "an evaluated captured frame enables Apply for the current predicate");
  notice.value(0); notice.do_callback();
  expect(!live.value() && std::string(status.value()).find("Stopped.") == 0,
         "withdrawing acknowledgement stops the capture session");
  control("Apply calibration")->do_callback();
  if (probe.refuse_once) {
    expect(Fl::modal() == modal &&
               std::string(status.value()).find("Not applied: test refusal") == 0,
           "a refused Apply keeps the dialog and its draft open with the reason");
    control("Apply calibration")->do_callback();
  }
  probe.delivered = true;
}
std::size_t open_fds() {
  return static_cast<std::size_t>(std::distance(
      std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator{}));
}
void calibration_mask_coalesced_pointer_strokes() {
  Fixture fixture;
  struct EventState {
    int x{Fl::e_x}, y{Fl::e_y}, key{Fl::e_keysym};
    ~EventState() { Fl::e_x = x; Fl::e_y = y; Fl::e_keysym = key; }
  } event_state;
  Fl_Window window(320, 320);
  editor::CalibrationPreview preview(0, 0, 320, 320);
  window.end();
  core::TemplateImage image;
  image.width = image.height = 32;
  image.pixels.resize(32 * 32);
  image.alpha.assign(32 * 32, 255);
  preview.mask = &image;
  preview.painted = [&](const double x, const double y, const bool opaque) {
    expect(editor::paint_template_mask(image, x, y, opaque), "paint valid source-image mask pixels");
  };
  const auto paint_rows = [&](const int tool) {
    preview.tool = tool;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    for (int row = 1; row < 32; row += 3) {
      Fl::e_x = 1; Fl::e_y = row * 10 + 5;
      expect(preview.handle(FL_PUSH) == 1, "begin a mask brush stroke");
      Fl::e_x = 319;
      expect(preview.handle(FL_DRAG) == 1 && preview.handle(FL_RELEASE) == 1,
             "coalesced motion joins the complete brush stroke");
    }
  };
  paint_rows(1);
  expect(std::all_of(image.alpha.begin(), image.alpha.end(), [](const auto alpha) { return alpha == 0; }),
         "sparse pointer events erase every pixel without holes between samples");
  paint_rows(2);
  expect(std::all_of(image.alpha.begin(), image.alpha.end(), [](const auto alpha) { return alpha == 255; }),
         "a fully erased draft remains repairable by the restore brush");
  preview.resize(0, 0, 0, 0);
  expect(preview.handle(FL_PUSH) == 0, "a collapsed preview refuses degenerate pointer coordinates");
}

void calibration_unavailable_and_owned_capture() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  expect(app.session.has_document(), "show opens the only private fixture");
  auto profile = app.session.document().profile();
  const auto id = profile.cooldowns.front().id;
  const auto before = open_fds();
  {
    CalibrationProbe probe;
    Fl::add_timeout(.01, exercise_calibration, &probe);
    auto seed = editor::CalibrationModel::open(profile, id, std::nullopt);
    const auto snapshot = app.session.document().profile();
    const auto generation = app.session.edit_generation();
    const auto owned = app.session.owned_template_assets().size();
    int unsampled_accepts{};
    const editor::CalibrationAccept accept =
        [&](const editor::CalibrationDraft &) -> editor::CalibrationAcceptance {
      ++unsampled_accepts;
      return std::string("unexpected apply");
    };
    const auto result = editor::run_x11_calibration(fixture.path, std::move(*seed), accept);
    Fl::remove_timeout(exercise_calibration, &probe);
    expect(probe.delivered && !result && unsampled_accepts == 0,
           "unavailable calibration Cancel returns no data and never accepts");
    expect(app.session.document().profile() == snapshot &&
               app.session.edit_generation() == generation &&
               app.session.owned_template_assets().size() == owned,
           "an unsampled Apply changes no document, generation or owned bytes");
  }
  expect(open_fds() == before, "unavailable modal releases its discovery display");
  struct X11Fixture {
    Display *display{XOpenDisplay(nullptr)};
    Window window{};
    X11Fixture() {
      expect(display != nullptr, "open only a disposable X11 fixture display");
      window = XCreateSimpleWindow(display, DefaultRootWindow(display), 1100, 780, 120, 100, 0, 0, 0);
      XStoreName(display, window, "Private calibration target");
      XClassHint hint{const_cast<char *>("LayerCCalibrationFixture"), const_cast<char *>("LayerCCalibrationFixture")};
      XSetClassHint(display, window, &hint);
      const unsigned long pid = static_cast<unsigned long>(::getpid());
      XChangeProperty(display, window, XInternAtom(display, "_NET_WM_PID", False),
                      XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<const unsigned char *>(&pid), 1);
      XMapWindow(display, window);
      auto gc = XCreateGC(display, window, 0, nullptr);
      for (int row = 0; row < 100; ++row) {
        XSetForeground(display, gc, static_cast<unsigned long>((row * 2) << 16 | (255 - row * 2) << 8 | row));
        XFillRectangle(display, window, gc, 0, row, 120, 1);
      }
      XFreeGC(display, gc); XSync(display, False);
    }
    ~X11Fixture() { if (display) { if (window) XDestroyWindow(display, window); XCloseDisplay(display); } }
  } target;
  profile.game.executable_name = std::filesystem::read_symlink("/proc/self/exe").filename().string();
  profile.game.window_class = "LayerCCalibrationFixture";
  const auto with_fixture = open_fds();
  {
    CalibrationProbe probe; probe.capture = true; probe.refuse_once = true;
    Fl::add_timeout(.01, exercise_calibration, &probe);
    auto seed = editor::CalibrationModel::open(profile, id, std::nullopt);
    int accepts{};
    const editor::CalibrationAccept accept =
        [&](const editor::CalibrationDraft &draft) -> editor::CalibrationAcceptance {
      if (++accepts == 1) return std::string("test refusal");
      expect(draft.images.size() == 1, "the retried Apply still carries the complete draft");
      if (!draft.images.empty())
        expect(core::template_image_has_structure(draft.images.front()) &&
                   !draft.images.front().asset_sha256.empty(),
               "captured reference has structure and canonical identity");
      editor::AcceptedCalibration accepted_value;
      accepted_value.visual = draft.visual;
      return accepted_value;
    };
    const auto result = editor::run_x11_calibration(fixture.path, std::move(*seed), accept);
    Fl::remove_timeout(exercise_calibration, &probe);
    expect(probe.delivered && accepts == 2 && result &&
               result->visual.observations.size() == 1 &&
               result->visual.triggers.size() == 1,
           "real X11 calibration returns the typed accepted module");
    expect(std::holds_alternative<core::TemplateState>(
               result->visual.observations.front()),
           "the accepted template calibration keeps its template observation");
  }
  expect(open_fds() == with_fixture, "completed calibration releases capture and discovery connections");
  {
    struct IdentityProbe {
      Display *display{}; Window window{}; int stage{}, attempts{}; bool delivered{};
      static void run(void *data) {
        auto &probe = *static_cast<IdentityProbe *>(data);
        auto *modal = Fl::modal();
        if (!modal) {
          if (++probe.attempts < 100) Fl::add_timeout(.01, run, data);
          return;
        }
        auto *live = dynamic_cast<Fl_Check_Button *>(find_control(*modal, "Live preview"));
        auto *notice = dynamic_cast<Fl_Check_Button *>(find_control(*modal,
            "I acknowledge passive capture of the selected game window"));
        auto *preview = dynamic_cast<editor::CalibrationPreview *>(find_control(*modal, "Calibration preview"));
        auto *status = dynamic_cast<Fl_Multiline_Output *>(find_control(*modal, "Capture / live match status"));
        expect(live && notice && preview && status, "identity probe uses the real modal controls");
        if (probe.stage++ == 0) {
          find_control(*modal, "Rescan windows")->do_callback();
          notice->value(1); notice->do_callback(); live->value(1); live->do_callback();
          expect(live->value() && preview->frame, "identity test begins with available live capture");
          const auto property = XInternAtom(probe.display, "_LAYERC_TEST_CAPTURE_CHURN", False);
          for (unsigned long value = 0; value < 3000; ++value)
            XChangeProperty(probe.display, probe.window, property, XA_CARDINAL, 32,
                            PropModeReplace, reinterpret_cast<const unsigned char *>(&value), 1);
          XStoreName(probe.display, probe.window, "Changed calibration identity");
          XSync(probe.display, False);
          Fl::add_timeout(.5, run, data); return;
        }
        expect(!live->value() && !notice->value() && !preview->frame &&
                   std::string(status->value()).find("changed identity") != std::string::npos,
               "live identity invalidation clears the frame and requires renewed capture consent");
        find_control(*modal, "Cancel calibration")->do_callback(); probe.delivered = true;
      }
    } probe{.display = target.display, .window = target.window};
    Fl::add_timeout(.01, IdentityProbe::run, &probe);
    auto seed = editor::CalibrationModel::open(profile, id, std::nullopt);
    const editor::CalibrationAccept accept =
        [](const editor::CalibrationDraft &) -> editor::CalibrationAcceptance {
      return std::string("unexpected apply");
    };
    const auto result = editor::run_x11_calibration(fixture.path, std::move(*seed), accept);
    Fl::remove_timeout(IdentityProbe::run, &probe);
    expect(probe.delivered && !result, "identity loss never returns an accepted calibration");
  }
  expect(open_fds() == with_fixture, "identity failure joins and releases its private connections");
  editor::EditorAppTestAccess::window(app).hide();
}


} // namespace

#ifdef LAYERC_TEST_XTEST
void linux_editor_focused_keyboard_capture_contracts();
#define LAYERC_FOCUSED_CAPTURE_CASE \
  , LAYERC_TEST_CASE(linux_editor_focused_keyboard_capture_contracts)
#else
#define LAYERC_FOCUSED_CAPTURE_CASE
#endif

LAYERC_TEST_MAIN(
  LAYERC_TEST_CASE(linux_editor_accessibility_tree_rows),
  LAYERC_TEST_CASE(linux_editor_diagnostic_text_contracts),
  LAYERC_TEST_CASE(linux_editor_command_registry_contracts),
  LAYERC_TEST_CASE(linux_editor_page_lists_clear_without_document),
  LAYERC_TEST_CASE(linux_editor_semantic_row_extents),
  LAYERC_TEST_CASE(linux_editor_field_error_contracts),
  LAYERC_TEST_CASE(linux_editor_pending_automation_status_contracts),
  LAYERC_TEST_CASE(linux_editor_calibration_layout_contracts),
  LAYERC_TEST_CASE(linux_editor_black_accent_dialog_contracts),
  LAYERC_TEST_CASE(linux_editor_main_window_text_and_tab_labels),
  LAYERC_TEST_CASE(linux_editor_text_views_fit_content),
  LAYERC_TEST_CASE(linux_editor_empty_automation_simulator_ready),
  LAYERC_TEST_CASE(linux_editor_empty_template_apply_is_not_an_edit),
  LAYERC_TEST_CASE(linux_editor_system_theme_restores_tab_class_colors),
  LAYERC_TEST_CASE(linux_editor_cooldown_group_dialog_transactions),
  LAYERC_TEST_CASE(linux_editor_export_profile_checklist),
  LAYERC_TEST_CASE(empty_draft_save_and_reopen),
  LAYERC_TEST_CASE(text_history_and_invalid_navigation),
  LAYERC_TEST_CASE(cooldowns_bindings_and_panels),
  LAYERC_TEST_CASE(dirty_cancel_discard_and_delete),
  LAYERC_TEST_CASE(capture_cancel_and_small_window),
  LAYERC_TEST_CASE(menus_follow_document_history_and_context),
  LAYERC_TEST_CASE(context_rows_and_fixed_menu_geometry),
  LAYERC_TEST_CASE(document_shortcuts_and_native_text_keys),
  LAYERC_TEST_CASE(routed_ctrl_z_precedes_field_undo),
  LAYERC_TEST_CASE(automation_history_and_invalid_properties),
  LAYERC_TEST_CASE(automation_json_and_simulator_lifetime),
  LAYERC_TEST_CASE(automation_trace_disconnect),
  LAYERC_TEST_CASE(visual_page_history_and_cancel),
  LAYERC_TEST_CASE(visual_remove_refuses_nested_selection_change),
  LAYERC_TEST_CASE(calibration_mask_coalesced_pointer_strokes),
  LAYERC_TEST_CASE(calibration_unavailable_and_owned_capture)
  LAYERC_FOCUSED_CAPTURE_CASE)
