// Modal dialog and appearance cases of the Linux editor widget suite, split from
// linux_editor_widget_tests.cpp to keep translation units focused.
#include "calibration_window.hpp"
#include "cooldown_groups_window.hpp"
#include "appearance_window.hpp"
#include "editor_app.hpp"
#include "editor_appearance.hpp"
#include "editor_menus.hpp"
#include "export_selection_window.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "linux_editor_test_access.hpp"
#include "profile_edit.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Window.H>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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

} // namespace

void linux_editor_cooldown_group_dialog_transactions() {
  Fixture fixture; fixture.store();
  editor::EditorSession session(fixture.path.parent_path());
  expect(!session.open(0), "open the private group fixture");
  const auto original = session.document().profile();
  {
    editor::CooldownGroupsDialog dialog(session); dialog.show(); Fl::check();
    find_button(dialog, "Add group")->do_callback();
    dialog.mode->value(1); dialog.mode->do_callback();
    dialog.duration->value("nan");
    find_button(dialog, "Apply")->do_callback();
    expect(dialog.shown() && !dialog.applied && session.document().profile() == original,
           "invalid group timing leaves the dialog and document intact");
    dialog.duration->value("2.5"); dialog.charges->value("3");
    dialog.recovery->value(1); dialog.retrigger->value(1);
    dialog.name->value("Shared charges");
    find_button(dialog, "Apply")->do_callback();
    expect(dialog.applied, "valid complete group edit applies");
  }
  const auto grouped = session.document().profile();
  expect(grouped.cooldown_groups.size() == 1 &&
             grouped.cooldown_groups.front().duration == core::Milliseconds(2500) &&
             grouped.cooldown_groups.front().max_charges == 3 &&
             grouped.cooldown_groups.front().recovery == core::RecoveryMode::sequential &&
             grouped.cooldown_groups.front().retrigger == core::RetriggerPolicy::restart,
         "the group dialog edits shared timing, charges, recovery and retrigger");
  expect(session.undo() && session.document().profile() == original &&
             session.redo() && session.document().profile() == grouped,
         "the entire group dialog is one undoable session edit");
  {
    editor::CooldownGroupsDialog dialog(session); dialog.show(); Fl::check();
    find_button(dialog, "Add group")->do_callback();
    dialog.mode->value(1); dialog.mode->do_callback();
    find_button(dialog, "Apply")->do_callback();
    expect(dialog.shown() && !dialog.applied && session.document().profile() == grouped,
           "overlapping shared pools are refused without changing the document");
    find_button(dialog, "Cancel")->do_callback();
  }
  expect(!session.edit([](auto &profile) {
    profile.cooldown_groups.front().extra_fields.emplace("future", core::json::Value(true));
  }), "add opaque group metadata to the fixture");
  {
    editor::CooldownGroupsDialog dialog(session); dialog.show(); Fl::check();
    dialog.name->value("Renamed group");
    find_button(dialog, "Apply")->do_callback();
    expect(dialog.applied &&
               session.document().profile().cooldown_groups.front().extra_fields.contains("future"),
           "group field edits retain opaque metadata");
  }
  {
    editor::CooldownGroupsDialog dialog(session); dialog.show(); Fl::check();
    find_button(dialog, "Remove group")->do_callback();
    find_button(dialog, "Apply")->do_callback();
    expect(dialog.applied && session.document().profile().cooldown_groups.empty(),
           "removing a group is an explicit validated edit");
  }
}

void linux_editor_export_profile_checklist() {
  Fixture fixture;
  std::vector<platform::StoredProfile> entries(3);
  for (std::size_t i{}; i < entries.size(); ++i) {
    entries[i].profile.id = "profile-" + std::to_string(i);
    entries[i].profile.name = "Same visible name";
  }
  {
    editor::ExportSelectionDialog dialog(entries, 1); dialog.show(); Fl::check();
    expect(dialog.profiles.size() == 3 && dialog.profiles[1]->value() &&
               !dialog.profiles[0]->value(),
           "the export checklist defaults to the selected saved profile");
    find_button(dialog, "Select none")->do_callback();
    find_button(dialog, "Export")->do_callback();
    expect(dialog.shown() && !dialog.selection, "an empty export selection is refused");
    dialog.profiles[0]->value(1); dialog.profiles[2]->value(1);
    find_button(dialog, "Export")->do_callback();
    expect(dialog.selection == std::vector<std::size_t>({0, 2}),
           "the export checklist accepts an arbitrary noncontiguous subset");
  }
  {
    editor::ExportSelectionDialog dialog(entries, 1); dialog.show(); Fl::check();
    find_button(dialog, "Select all")->do_callback();
    expect(std::ranges::all_of(dialog.profiles, [](auto *item) { return item->value(); }),
           "Select all includes every profile");
    find_button(dialog, "Cancel")->do_callback();
    expect(!dialog.selection, "Cancel never returns an export intent");
  }
}

void linux_editor_system_theme_restores_tab_class_colors() {
  // FLTK picks its backend once per process; the first case must pin X11.
  Fixture fixture;
  Fl_Window window(400, 300);
  Fl_Tabs tabs(0, 0, 400, 300);
  Fl_Group first(0, 25, 400, 275, "First");
  Fl_Hold_Browser list(10, 60, 100, 100); first.end();
  Fl_Group second(0, 25, 400, 275, "Second"); second.end();
  tabs.end(); window.end();
  const auto background = Fl::get_color(FL_BACKGROUND_COLOR);
  const auto foreground = Fl::get_color(FL_FOREGROUND_COLOR);
  const auto selection = Fl::get_color(FL_SELECTION_COLOR);
  editor::EditorAppearance themed; themed.theme = editor::EditorTheme::teal;
  editor::apply_editor_appearance(themed, window);
  editor::apply_editor_appearance({}, window);
  Fl_Widget *tab_widgets[]{&tabs, &first, &second};
  for (auto *widget : tab_widgets) {
    expect(Fl::get_color(widget->selection_color()) == background,
           "System restores the default tab background, not the list selection colour");
    expect(Fl::get_color(widget->labelcolor()) == foreground,
           "System restores readable foreground text on every tab");
  }
  expect(Fl::get_color(list.selection_color()) == selection,
         "System preserves the native list highlight colour");
}
