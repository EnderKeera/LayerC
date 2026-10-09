// Deterministic tests of the actual FLTK frontend, not native-input evidence.
// All writes/control attempts are in private fixtures. Desktop delivery and
// accessibility are covered separately by the disposable GUI walkthrough.
#include "editor_app.hpp"
#include "profile_edit.hpp"
#include "test_support.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Tabs.H>
#include <sys/stat.h>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

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
      choice.delivered = true; button->do_callback(); return;
    }
    if (++choice.attempts >= 100) { modal->hide(); return; }
  } else if (++choice.attempts >= 100) return;
  Fl::add_timeout(.01, reply, data);
}
class Answer final {
public:
  explicit Answer(std::string label) : choice_{std::move(label), 0, false} {
    Fl::add_timeout(.01, reply, &choice_);
  }
  ~Answer() { Fl::remove_timeout(reply, &choice_); }
  [[nodiscard]] bool delivered() const { return choice_.delivered; }
private:
  Reply choice_;
};
void show(editor::EditorApp &app) { app.build(); app.window.show(); Fl::check(); }
void tab(editor::EditorApp &app, Fl_Group *group) {
  app.tabs->value(group); app.tab_changed();
}

void empty_draft_save_and_reopen() {
  Fixture fixture;
  editor::EditorApp app;
  show(app);
  expect(!app.session.has_document() && !app.tabs->active(),
         "an empty store has no phantom open document");
  app.command(editor::EditorCommand::new_profile);
  expect(app.session.is_draft() && app.tabs->active(), "New enables the draft controls");
  app.name->value("Widget draft"); app.field_changed(app.name);
  expect(!app.save() && !std::filesystem::exists(fixture.path),
         "an unmatched draft fails visibly without writing");
  app.executable->value("widget.exe"); app.field_changed(app.executable);
  expect(app.save() && !app.session.has_unsaved_changes(),
         "explicit Save commits the accepted draft");
  const auto path = app.session.document().path();
  expect(std::string(app.status->value()).find("LayerD is not running") != std::string::npos,
         "the durable Save separately reports the private daemon absent");
  expect(!app.session.open_path(path) && app.session.document().profile().name == "Widget draft",
         "the saved draft reopens with the accepted fields");
  app.window.hide();
}
void text_history_and_invalid_navigation() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  const auto original = app.session.document().profile().name;
  app.name->value("First"); app.field_changed(app.name);
  app.name->value("First edited"); app.field_changed(app.name);
  app.command(editor::EditorCommand::undo);
  expect(app.session.document().profile().name == original, "one Undo restores coalesced typing");
  app.command(editor::EditorCommand::redo);
  expect(app.session.document().profile().name == "First edited", "Redo restores accepted typing");
  tab(app, app.cooldown_tab);
  const auto duration = app.session.document().profile().cooldowns.front().duration;
  app.duration->value("nan"); app.field_changed(app.duration);
  expect(std::string(app.status->value()).find("Invalid input") != std::string::npos &&
         app.session.document().profile().cooldowns.front().duration == duration,
         "invalid numeric text stays visible without modifying accepted data");
  tab(app, app.panel_tab);
  expect(app.tabs->value() == app.cooldown_tab && std::string(app.duration->value()) == "nan",
         "navigation cannot silently discard an invalid field buffer");
  app.command(editor::EditorCommand::undo);
  expect(app.session.document().profile().cooldowns.front().duration == duration &&
         std::string(app.duration->value()) != "nan",
         "Undo explicitly discards the locally invalid field");
  app.window.hide();
}
void cooldowns_bindings_and_panels() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  tab(app, app.cooldown_tab);
  const auto count = app.session.document().profile().cooldowns.size();
  app.command(editor::EditorCommand::add_cooldown);
  expect(app.session.document().profile().cooldowns.size() == count + 1,
         "Add cooldown edits the accepted document");
  app.cooldown_name->value("A/B & Unicode Δ"); app.field_changed(app.cooldown_name);
  app.charges->value("2"); app.field_changed(app.charges);
  app.recovery->value(1); app.field_changed(app.recovery);
  expect(app.session.document().profile().cooldowns.back().name == "A/B & Unicode Δ" &&
         app.session.document().profile().cooldowns.back().max_charges == 2,
         "cooldown text and charge/recovery fields are applied");
  tab(app, app.binding_tab);
  expect(app.binding_target->size() == static_cast<int>(count + 2),
         "authored slash/ampersand names remain one choice per cooldown");
  app.binding_target->value(static_cast<int>(count));
  const auto binding_count = app.session.document().profile().bindings.size();
  app.key_code->value("18"); app.command(editor::EditorCommand::add_keyboard);
  app.mouse_button->value(5); app.command(editor::EditorCommand::add_mouse);
  const auto &bindings = app.session.document().profile().bindings;
  expect(bindings.size() == binding_count + 2 &&
         bindings.back().chord.tokens.front().code == 6,
         "keyboard and wheel choices create their real profile tokens");
  tab(app, app.panel_tab);
  const auto panel_count = app.session.document().profile().panels.size();
  app.command(editor::EditorCommand::add_panel);
  app.anchor_x->value("0.25"); app.field_changed(app.anchor_x);
  app.fill->value("#123456"); app.field_changed(app.fill);
  expect(app.session.document().profile().panels.size() == panel_count + 1 &&
         app.session.document().profile().panels.back().anchor_x == .25 &&
         app.session.document().profile().panels.back().bar.fill_color == 0x123456,
         "panel creation, local placement and color controls use accepted edits");
  app.fill->value("not-a-color"); app.field_changed(app.fill);
  expect(!app.save() && std::string(app.fill->value()) == "not-a-color",
         "Save refuses an invalid panel color and retains the field buffer");
  app.window.hide();
}
void dirty_cancel_discard_and_delete() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  app.name->value("Unsaved"); app.field_changed(app.name);
  const auto before = layerc::test::read_file(fixture.path);
  {
    Answer answer("Cancel");
    expect(!app.may_leave(), "Cancel blocks leaving the dirty profile");
    expect(answer.delivered(), "Cancel was delivered to the real modal dialog");
  }
  expect(app.session.document().profile().name == "Unsaved" &&
         layerc::test::read_file(fixture.path) == before, "cancel keeps accepted edits and the disk unchanged");
  {
    Answer answer("Discard");
    expect(app.may_leave(), "Discard reloads the saved document");
    expect(answer.delivered(), "Discard was delivered to the real modal dialog");
  }
  expect(!app.session.has_unsaved_changes() && layerc::test::read_file(fixture.path) == before,
         "discarding an edit never writes the store");
  {
    Answer answer("Cancel");
    app.command(editor::EditorCommand::delete_profile);
    expect(answer.delivered() && std::filesystem::exists(fixture.path),
           "canceled deletion leaves the profile on disk");
  }
  {
    Answer answer("Delete");
    app.command(editor::EditorCommand::delete_profile);
    expect(answer.delivered() && !std::filesystem::exists(fixture.path) &&
           !app.session.has_document(), "confirmed deletion uses the real profile transaction");
  }
  app.window.hide();
}
void capture_cancel_and_small_window() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  tab(app, app.binding_tab);
  const auto before = app.session.document().profile();
  {
    Answer answer("Cancel");
    app.command(editor::EditorCommand::capture_binding);
    expect(answer.delivered(), "focused capture opens its owned cancelable dialog");
  }
  expect(app.session.document().profile() == before, "canceled capture changes no binding");
  app.window.size(640, 480); Fl::check();
  expect(app.content->w() == 640 && app.content->h() == 480 - editor::EditorMenus::bar_height &&
         app.status->w() == 595 && app.panel_name->w() == 260,
         "a small viewport scrolls intact forms rather than squeezing them into overlap");
  app.content->scroll_to(0, 300); Fl::check();
  expect(app.content->yposition() > 0, "the small viewport exposes the bottom status area by scrolling");
  app.window.hide();
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
  auto &bar = app.menus->bar();
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
  app.executable->value("menu.exe"); app.field_changed(app.executable);
  activate_menu(bar, "&File/&Save");
  expect(!app.session.is_draft() && !app.session.has_unsaved_changes() &&
         menu_item(bar, "&Profile/&Delete...").active(), "menu Save durably publishes the draft");
  const auto original = app.session.document().profile().name;
  app.name->value("Menu edit"); app.field_changed(app.name);
  expect(menu_item(bar, "&Edit/&Undo").active(), "accepted text updates menu history state");
  activate_menu(bar, "&Edit/&Undo");
  expect(app.session.document().profile().name == original &&
         menu_item(bar, "&Edit/&Redo").active(), "menu Undo restores data and enables Redo");
  activate_menu(bar, "&Edit/&Redo");
  expect(app.session.document().profile().name == "Menu edit", "menu Redo restores the accepted edit");
  Fl_Menu_Button context(0, 0, 0, 0);
  const auto before = app.session.document().profile();
  app.menus->configure_context(context, editor::EditorMenuContext::profiles, false);
  expect(!menu_item(context, "&Duplicate").active() &&
         menu_item(context, "&New profile").active(), "empty-area context does not inherit selected actions");
  activate_menu(context, "&Duplicate");
  expect(app.session.document().profile() == before && !app.session.is_draft(),
         "a disabled empty-area callback cannot duplicate the previous selection");
  app.menus->configure_context(context, editor::EditorMenuContext::profiles, true);
  expect(menu_item(context, "&Duplicate").active(), "selected-row context enables its own commands");
  {
    Answer answer("Cancel");
    activate_menu(context, "&Delete...");
    expect(answer.delivered() && std::filesystem::exists(app.session.document().path()),
           "menu deletion obeys dirty-cancel and preserves the store");
  }
  activate_menu(bar, "&View/&Cooldowns");
  app.duration->value("nan"); app.field_changed(app.duration);
  activate_menu(bar, "&View/&Panels");
  expect(app.tabs->value() == app.cooldown_tab &&
         std::string(app.duration->value()) == "nan", "menu navigation retains an invalid local field buffer");
  activate_menu(bar, "&Edit/&Undo");
  expect(std::string(app.duration->value()) != "nan", "menu Undo explicitly discards invalid text");
  app.window.hide();
}
void context_rows_and_fixed_menu_geometry() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  auto *list = dynamic_cast<editor::ContextBrowser *>(app.profiles);
  expect(list != nullptr &&
         list->row_at(list->x() + 10, list->y() + 10) == 1 &&
         list->row_at(list->x() + 10, list->y() + list->h() - 10) == 0,
         "FLTK row bounds distinguish a real row from empty list space");
  const auto row = list->row_bounds(1);
  expect(row && row->height > 0 && row->height < list->h() && row->contains(list->x() + 10, list->y() + 10) &&
         !list->row_bounds(list->size() + 1),
         "accessible row extents are one visible row, not the whole browser");
  auto *tabs = dynamic_cast<editor::EditorTabs *>(app.tabs);
  const auto first = tabs ? tabs->tab_bounds(0) : std::nullopt;
  const auto second = tabs ? tabs->tab_bounds(1) : std::nullopt;
  expect(first && second && first->x + first->width <= second->x + 1 && first->height < 40 &&
         tabs->which(first->x + first->width / 2, first->y + first->height / 2) == app.profile_tab &&
         tabs->which(second->x + second->width / 2, second->y + second->height / 2) == app.cooldown_tab &&
         !tabs->tab_bounds(tabs->children()),
         "accessible tab extents match FLTK's own tab hit testing");
  auto &bar = app.menus->bar();
  app.window.size(640, 480); Fl::check();
  const int bar_y = bar.y();
  app.content->scroll_to(0, 200); Fl::check();
  expect(bar_y == 0 && bar.y() == 0 && bar.w() == 640 &&
         app.content->y() == editor::EditorMenus::bar_height &&
         app.content->h() == 480 - editor::EditorMenus::bar_height,
         "the top-level menu remains fixed above the small scrolling viewport");
  Fl_Menu_Button context(0, 0, 0, 0);
  app.menus->configure_context(context, editor::EditorMenuContext::cooldowns, false);
  expect(menu_item(context, "&Add cooldown").active() &&
         !menu_item(context, "&Remove cooldown...").active(),
         "empty cooldown context permits Add but not removal of a previous row");
  app.menus->configure_context(context, editor::EditorMenuContext::cooldowns, true);
  const auto before = app.session.document().profile().cooldowns.size();
  activate_menu(context, "&Add cooldown");
  expect(app.session.document().profile().cooldowns.size() == before + 1,
         "context Add uses the same validated command as the button");
  app.window.hide();
}
void document_shortcuts_and_native_text_keys() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  const auto original = app.session.document().profile().name;
  Fl::focus(app.name);
  app.name->value("Shortcut edit"); app.field_changed(app.name);
  expect(app.menus->shortcut('z', FL_CTRL) &&
         app.session.document().profile().name == original,
         "document Ctrl+Z uses accepted history with a text field focused");
  expect(app.menus->shortcut('Z', FL_CTRL | FL_SHIFT) &&
         app.session.document().profile().name == "Shortcut edit",
         "Ctrl+Shift+Z restores the accepted document, not a widget-only buffer");
  expect(app.menus->shortcut('z', FL_CTRL) && app.menus->shortcut('y', FL_CTRL) &&
         app.session.document().profile().name == "Shortcut edit",
         "Ctrl+Y redoes like the Windows editor");
  expect(!app.menus->shortcut('a', FL_CTRL) && !app.menus->shortcut('c', FL_CTRL) &&
         !app.menus->shortcut('v', FL_CTRL) && !app.menus->shortcut('e', FL_CTRL) &&
         !app.menus->shortcut('z', FL_CTRL | FL_ALT),
         "native select/copy/paste/end and unmatched modifier combinations are not intercepted");
  expect(app.menus->shortcut('s', FL_CTRL) && !app.session.has_unsaved_changes() &&
         layerc::test::read_file(fixture.path).value_or("").find("Shortcut edit") != std::string::npos,
         "the shared shortcut dispatcher durably saves accepted edits");
  app.window.hide();
}

// A real key event reaches the focused input before its window; document
// undo must still win over the field's own local undo buffer.
void routed_ctrl_z_precedes_field_undo() {
  Fixture fixture; fixture.store();
  editor::EditorApp app; show(app);
  const auto original = app.session.document().profile().game.executable_name;
  Fl::focus(app.executable);
  app.executable->insert_position(app.executable->size());
  (void)app.executable->replace(app.executable->size(), app.executable->size(), "x", 1);
  app.field_changed(app.executable);
  expect(app.apply_fields() && app.session.document().profile().game.executable_name == original + "x",
         "the typed executable name is an accepted document edit");
  Fl::e_keysym = 'z';
  Fl::e_state = FL_CTRL;
  Fl::e_text = const_cast<char *>("\x1a");
  Fl::e_length = 1;
  expect(Fl::handle(FL_KEYBOARD, &app.window) != 0 &&
         app.session.document().profile().game.executable_name == original,
         "a dispatched Ctrl+Z undoes the document edit, not only the field's text");
  Fl::e_state = 0;
  app.window.hide();
}
} // namespace

LAYERC_TEST_MAIN(
  LAYERC_TEST_CASE(empty_draft_save_and_reopen),
  LAYERC_TEST_CASE(text_history_and_invalid_navigation),
  LAYERC_TEST_CASE(cooldowns_bindings_and_panels),
  LAYERC_TEST_CASE(dirty_cancel_discard_and_delete),
  LAYERC_TEST_CASE(capture_cancel_and_small_window),
  LAYERC_TEST_CASE(menus_follow_document_history_and_context),
  LAYERC_TEST_CASE(context_rows_and_fixed_menu_geometry),
  LAYERC_TEST_CASE(document_shortcuts_and_native_text_keys),
  LAYERC_TEST_CASE(routed_ctrl_z_precedes_field_undo))
