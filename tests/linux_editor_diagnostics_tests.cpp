// F17 diagnostics: the panel-colours layout, the field-specific status text and
// the pending-automation validation box the Linux editor reports.
#include "accessibility_tree.hpp"
#include "automation_editor.hpp"
#include "automation_graph.hpp"
#include "editor_app.hpp"
#include "editor_menus.hpp"
#include "editor_tools.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/core/profile.hpp"
#include "profile_edit.hpp"
#include "linux_editor_test_access.hpp"
#include "test_support.hpp"

namespace editor = layerc::editor;

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Output.H>

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace {

using layerc::test::expect;

// A disposable XDG environment with one stored fixture profile, mirroring the
// widget-suite fixture.
class Fixture final {
public:
  Fixture()
      : temp_("layerc-editor-diagnostics"), home_("HOME", temp_.path()),
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
    path = layerc::platform::profile_directory() / "fixture.json";
  }
  void store() {
    auto profile = layerc::editor::make_new_profile();
    profile.game.executable_name = "fixture.exe";
    expect(layerc::platform::write_profile_file(
               path, layerc::core::serialize_profile_json(profile))
               .ok(),
           "store only a private fixture");
  }
  std::filesystem::path path;

private:
  layerc::test::TempDirectory temp_;
  layerc::test::ScopedEnvironment home_, config_, data_, runtime_, cache_, state_,
      override_, store_override_, bus_, accessibility_bus_, backend_;
};

void show(layerc::editor::EditorApp &app) {
  app.build();
  editor::EditorAppTestAccess::window(app).show();
  Fl::check();
}

void tab(layerc::editor::EditorApp &app, Fl_Group *group) {
  editor::EditorAppTestAccess::tabs(app)->value(group);
  app.tab_changed();
}

template <typename Widget>
Widget *find_typed(Fl_Group &group, const std::string_view label) {
  for (int i = 0; i < group.children(); ++i) {
    auto *child = group.child(i);
    if (auto *typed = dynamic_cast<Widget *>(child);
        typed && child->label() && label == child->label())
      return typed;
    if (auto *nested = dynamic_cast<Fl_Group *>(child))
      if (auto *found = find_typed<Widget>(*nested, label)) return found;
  }
  return nullptr;
}

template <typename Widget>
Widget &automation_control(layerc::editor::EditorApp &app,
                           const std::string_view label) {
  auto *widget = find_typed<Widget>(*editor::EditorAppTestAccess::automation(app)->group(), label);
  expect(widget != nullptr, "the automation page owns the named control: " + std::string(label));
  return *widget;
}

} // namespace

void linux_editor_diagnostic_text_contracts() {
  namespace editor = layerc::editor;
  setenv("FLTK_BACKEND", "x11", 1);
  const auto layout = editor::panel_colors_layout();
  expect(layout.label_width > 0 && 15 + layout.label_width <= layout.input_x - 12,
         "the widest label fits left of the first input");
  expect(layout.button_x >= layout.input_x + 140,
         "the chooser column follows the input column");
  expect(layout.window_width >= layout.button_x + 155 + 15,
         "the window is wide enough for both columns and the trailing margin");
}

void linux_editor_field_error_contracts() {
  namespace editor = layerc::editor;
  Fixture fixture;
  fixture.store();
  editor::EditorApp app;
  show(app);
  tab(app, editor::EditorAppTestAccess::cooldown_tab(app));
  const auto charges = app.session.document().profile().cooldowns.front().max_charges;
  editor::EditorAppTestAccess::charges(app)->value("0");
  app.field_changed(editor::EditorAppTestAccess::charges(app));
  expect(std::string(editor::EditorAppTestAccess::status(app)->value()).find(
             "Invalid input: Max charges must be between 1 and 99.") !=
                 std::string::npos &&
             app.session.document().profile().cooldowns.front().max_charges ==
                 charges,
         "an out-of-range charge count names the field and keeps the profile");
  app.command(editor::EditorCommand::undo);
  tab(app, editor::EditorAppTestAccess::panel_tab(app));
  const auto anchor = app.session.document().profile().panels.front().anchor_x;
  editor::EditorAppTestAccess::anchor_x(app)->value("2");
  app.field_changed(editor::EditorAppTestAccess::anchor_x(app));
  expect(std::string(editor::EditorAppTestAccess::status(app)->value()).find(
             "Invalid input: Anchor X must be between 0 and 1.") !=
                 std::string::npos &&
             app.session.document().profile().panels.front().anchor_x == anchor,
         "an out-of-range anchor names the field and keeps the profile");
  editor::EditorAppTestAccess::window(app).hide();
}

// Closing the document must clear the page lists, not leave stale rows behind.

void linux_editor_page_lists_clear_without_document() {
  namespace editor = layerc::editor;
  Fixture fixture;
  fixture.store();
  editor::EditorApp app;
  show(app);
  expect(app.session.has_document() &&
             editor::EditorAppTestAccess::cooldowns(app)->size() > 0 &&
             editor::EditorAppTestAccess::bindings(app)->size() > 0 &&
             editor::EditorAppTestAccess::panels(app)->size() > 0,
         "the fixture fills the cooldown, binding and panel lists");
  app.session.close_document();
  app.refresh();
  expect(!app.session.has_document() &&
             editor::EditorAppTestAccess::cooldowns(app)->size() == 0 &&
             editor::EditorAppTestAccess::bindings(app)->size() == 0 &&
             editor::EditorAppTestAccess::panels(app)->size() == 0,
         "closing the document clears every page list");
  editor::EditorAppTestAccess::window(app).hide();
}

void linux_editor_pending_automation_status_contracts() {
  namespace editor = layerc::editor;
  Fixture fixture;
  fixture.store();
  editor::EditorApp app;
  show(app);
  app.command(editor::EditorCommand::automation_page);
  tab(app, editor::EditorAppTestAccess::automation(app)->group());
  editor::EditorAppTestAccess::automation(app)->refresh();
  auto &json = automation_control<Fl_Multiline_Input>(app, "Advanced automation JSON");
  auto &validation = automation_control<Fl_Output>(app, "Automation validation");
  json.value("{");
  json.do_callback();
  expect(std::string(validation.value()).starts_with(
             "Pending JSON is invalid and not applied: "),
         "an invalid pending buffer describes itself in the validation box");
  const auto pending_text = std::string(validation.value());
  automation_control<Fl_Button>(app, "Validate / Apply JSON").do_callback();
  expect(std::string(editor::EditorAppTestAccess::status(app)->value()).find("Invalid automation JSON:") !=
                 std::string::npos &&
             std::string(validation.value()) == pending_text,
         "a refused apply keeps the pending-buffer text visible");
  editor::EditorAppTestAccess::automation(app)->refresh();
  const auto &document = app.session.document();
  expect(std::string(validation.value()) ==
             editor::automation_validation_summary(document.automation(),
                                                   document.profile()),
         "discarding the buffer restores the committed summary");
  editor::EditorAppTestAccess::window(app).hide();
}

void linux_editor_semantic_row_extents() {
  namespace a11y = layerc::editor::accessibility;
  Fixture fixture;
  fixture.store();
  editor::EditorApp app;
  show(app);
  editor::EditorAppTestAccess::release_accessibility(app);
  auto &window = editor::EditorAppTestAccess::window(app);
  auto *list = dynamic_cast<editor::ContextBrowser *>(
      editor::EditorAppTestAccess::profiles(app));
  if (!expect(list != nullptr, "production profile list is a ContextBrowser")) return;
  list->clear();
  for (int i = 1; i <= 80; ++i)
    list->add(("Semantic row " + std::to_string(i)).c_str());
  list->topline(1);
  list->redraw();
  Fl::check();
  Fl_Menu_Bar *bar = nullptr;
  std::function<void(Fl_Group &)> locate = [&](Fl_Group &group) {
    for (int i = 0; i < group.children() && !bar; ++i) {
      if (auto *found = dynamic_cast<Fl_Menu_Bar *>(group.child(i))) bar = found;
      else if (auto *nested = dynamic_cast<Fl_Group *>(group.child(i))) locate(*nested);
    }
  };
  locate(window);
  if (!expect(bar != nullptr, "the editor window owns a menu bar")) return;
  {
    a11y::AccessibilityTree tree;
    std::string fault;
    if (!expect(tree.initialize(fault), "row geometry tree initializes")) return;
    tree.reconcile(true);
    std::function<AtkObject *(AtkObject *, Fl_Widget *)> find =
        [&](AtkObject *object, Fl_Widget *widget) -> AtkObject * {
      auto &node = a11y::node(object);
      if (node.kind == a11y::Kind::widget && node.widget() == widget) return object;
      for (auto *child : node.children)
        if (auto *found = find(child, widget)) return found;
      return nullptr;
    };
    auto *browser = find(atk_get_root(), list);
    auto *menu_object = find(atk_get_root(), bar);
    if (!expect(browser && menu_object, "browser and menu are exported")) return;
    const auto unref = [](AtkObject *p) { if (p) g_object_unref(p); };
    using Ref = std::unique_ptr<AtkObject, decltype(unref)>;
    Ref first(atk_object_ref_accessible_child(browser, 0), unref);
    Ref second(atk_object_ref_accessible_child(browser, 1), unref);
    Ref last(atk_object_ref_accessible_child(browser, 79), unref);
    Ref item(atk_object_ref_accessible_child(menu_object, 0), unref);
    if (!expect(first && second && last && item, "rows and menu item are reachable")) return;
    const auto bounds = [](AtkObject *object, AtkCoordType coordinates = ATK_XY_SCREEN) {
      std::array<gint, 4> value{};
      atk_component_get_extents(ATK_COMPONENT(object), &value[0], &value[1],
                                &value[2], &value[3], coordinates);
      return value;
    };
    const auto showing = [](AtkObject *object) {
      auto *state = atk_object_ref_state_set(object);
      const bool result = atk_state_set_contains_state(state, ATK_STATE_SHOWING) != 0;
      g_object_unref(state);
      return result;
    };
    const auto owner = bounds(browser), one = bounds(first.get()), two = bounds(second.get());
    expect(one[2] > 0 && one[3] > 0 && two[2] > 0 && two[3] > 0 &&
               one[1] + one[3] <= two[1] && one != two,
           "semantic rows have positive distinct ordered nonoverlapping extents");
    for (const auto &row : {one, two})
      expect(row[0] >= owner[0] && row[1] >= owner[1] &&
                 row[0] + row[2] <= owner[0] + owner[2] &&
                 row[1] + row[3] <= owner[1] + owner[3],
             "visible row extents stay within the browser viewport");
    const auto parent = bounds(first.get(), ATK_XY_PARENT);
    expect(parent[0] == one[0] - owner[0] && parent[1] == one[1] - owner[1] &&
               parent[2] == one[2] && parent[3] == one[3],
           "row parent coordinates are relative to its browser");
    const auto title = bounds(item.get()), bar_bounds = bounds(menu_object);
    expect(title[2] > 0 && title[2] < bar_bounds[2] && title[3] > 0 &&
               title[0] >= bar_bounds[0] && title[0] + title[2] <= bar_bounds[0] + bar_bounds[2],
           "closed menu-bar titles retain their own header geometry, not the whole bar");
    list->topline(80);
    list->redraw();
    Fl::check();
    tree.sync();
    expect(bounds(first.get()) == std::array<gint, 4>{} && !showing(first.get()),
           "a scrolled-out production row is zero-sized and not showing");
    const auto final = bounds(last.get());
    expect(final[2] > 0 && final[3] > 0 && showing(last.get()),
           "scrolling reveals the final row's own geometry");
  }
  window.hide();
  Fl::check();
}
