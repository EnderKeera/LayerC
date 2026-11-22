#include "a11y/fltk_semantics.hpp"
#include "editor_app.hpp"
#include "editor_menus.hpp"
#include "editor_popup.hpp"
#include "editor_style.hpp"
#include "platform/editor_options.hpp"
#include "platform/win32/window_settings.hpp"
#include "layerc/platform/settings_store.hpp"
#include "test_support.hpp"

#include <FL/Fl_Input.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Tooltip.H>
#include <windows.h>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace editor = layerc::editor;
namespace platform = layerc::platform;
using layerc::test::expect;

namespace {

class ControlsFixture {
public:
  ControlsFixture()
      : key_(L"Software\\LayerC.FltkControlsTests." + std::to_wstring(GetCurrentProcessId()) +
             L"." + std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(++sequence_)),
        settings_("LAYERC_SETTINGS_KEY", std::filesystem::path(key_)),
        profiles_("LAYERC_PROFILE_DIR", directory_.path()) {}
  ~ControlsFixture() {
    const auto removed = RegDeleteTreeW(HKEY_CURRENT_USER, key_.c_str());
    expect(removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND, "remove fixture key");
  }
  [[nodiscard]] bool ready() const { return settings_.applied() && profiles_.applied(); }
private:
  static inline unsigned sequence_{};
  std::wstring key_;
  layerc::test::TempDirectory directory_{"LayerC-FltkControlsTests"};
  layerc::test::ScopedEnvironment settings_, profiles_;
};

Fl_Widget *find_widget(Fl_Group &root, const std::string_view label) {
  for (int i = 0; i < root.children(); ++i) {
    auto *widget = root.child(i);
    if (widget->label() && label == widget->label()) return widget;
    if (auto *group = widget->as_group())
      if (auto *match = find_widget(*group, label)) return match;
  }
  return nullptr;
}

int handle_event(Fl_Widget &widget, const int event, const int key = 0,
                 const int modifiers = 0, const int dy = 0) {
  const int prior_key = Fl::e_keysym, prior_state = Fl::e_state, prior_dy = Fl::e_dy;
  Fl::e_keysym = key;
  Fl::e_state = modifiers;
  Fl::e_dy = dy;
  const int handled = widget.handle(event);
  Fl::e_keysym = prior_key;
  Fl::e_state = prior_state;
  Fl::e_dy = prior_dy;
  return handled;
}

bool switch_to(editor::EditorApp &app, const std::string_view label) {
  auto *page = find_widget(*app.main_window(), label);
  auto *tabs = page ? dynamic_cast<Fl_Tabs *>(page->parent()) : nullptr;
  if (!expect(tabs, "editor tab container")) return false;
  tabs->value(page);
  app.tab_changed();
  return true;
}

} // namespace

void editor_tooltips_keys_and_menu() {
  const ControlsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  editor::install_editor_style();
  editor::EditorApp app;
  app.build();
  auto &window = *app.main_window();
  const std::array tips{
      std::pair{"&New", "New profile (Ctrl+N)"},
      std::pair{"Delete", "Delete profile"},
      std::pair{"Executable path (optional)", "No application chosen yet"},
      std::pair{"Capture focused chord...", "Press the key or chord that starts this cooldown"},
      std::pair{"Capture HID binding...", "Bind a control on a generic HID device"},
      std::pair{"Position on window...", "Place the bars directly on the running game window"},
      std::pair{"Duration (seconds)", "Up / Down or the mouse wheel step 0.1 s (Shift: 1 s)"},
      std::pair{"Bar width", "Up / Down or the mouse wheel step 1 px"},
      std::pair{"Bar height", "Up / Down or the mouse wheel step 1 px"},
      std::pair{"Local panel preview", "Drag the selected bar. Arrow keys nudge it 1 px (Shift: 10 px)."},
  };
  for (const auto &[label, tip] : tips) {
    auto *widget = find_widget(window, label);
    // The capture note under the button joins its tooltip in the description.
    const auto described = std::string_view(label) == "Capture focused chord..."
        ? std::string(tip) + ". Capture reads only events in its focused editor dialog."
        : std::string(tip);
    expect(widget && widget->tooltip() && std::string_view(widget->tooltip()) == tip &&
               editor::a11y::description({widget, editor::a11y::TargetKind::widget}) == described,
           label);
  }
  const auto keys = editor::editor_list_shortcuts();
  const auto help = editor::shortcut_help();
  expect(keys.size() == 4 && keys[0].shortcut == FL_Delete && keys[1].shortcut == FL_Delete &&
             keys[2].shortcut == (FL_SHIFT | (FL_F + 10)) && keys[3].shortcut == FL_Menu,
         "registry scopes both Delete commands and both context-menu keys");
  for (const auto &key : keys)
    expect(help.find(std::string(key.keys) + '\t' + key.label) != std::string::npos,
           "keyboard help includes the scoped shortcut and its action");
  auto *bar = dynamic_cast<Fl_Menu_Bar *>(window.child(0));
  if (!expect(bar, "main menu bar")) return;
  const auto *size = bar->find_item("&View/&Text size/11&5 %");
  const auto *default_size = bar->find_item("&View/&Text size/&100 %");
  if (!expect(size && size->radio() && default_size && default_size->value(), "Windows has a checked text-size radio menu")) return;
  const editor::WindowsTextScaleShortcuts shortcuts([&](auto kind, auto text) { app.report(kind, std::move(text)); });
  size->do_callback(bar);
  expect(editor::current_editor_text_scale() == 115 && platform::read_editor_text_scale().value == 115 && size->value(),
         "View > Text size shares the persistent keyboard path and updates the radio check");
  default_size->do_callback(bar);
  app.show();
  app.command(editor::EditorCommand::new_profile);
  auto *profiles = dynamic_cast<editor::ContextBrowser *>(find_widget(window, "Profiles"));
  auto *name = dynamic_cast<Fl_Input *>(find_widget(window, "Profile name"));
  if (!expect(profiles && name, "profile list and name edit")) return;
  name->take_focus();
  expect(handle_event(*profiles, FL_KEYDOWN, FL_Delete) == 0, "Delete does not dispatch from a focused field");
  profiles->take_focus();
  for (const auto &[key, mods] : {std::pair{FL_F + 10, FL_SHIFT}, std::pair{FL_Menu, 0}}) {
    bool opened{};
    const auto close_popup = +[](void *data) {
      *static_cast<bool *>(data) = editor::popup_owner() != nullptr;
      if (auto *popup = Fl::grab()) (void)handle_event(*popup, FL_KEYDOWN, FL_Escape);
    };
    Fl::add_timeout(0.0, close_popup, &opened);
    const int handled = handle_event(*profiles, FL_KEYDOWN, key, mods);
    Fl::remove_timeout(close_popup, &opened);
    expect(handled && opened, "focused list opens its existing context popup from the keyboard");
  }
  window.hide();
}

void stepped_fields_and_preview_nudge() {
  const ControlsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  editor::EditorApp app;
  app.build();
  app.show();
  app.command(editor::EditorCommand::new_profile);
  auto &window = *app.main_window();
  auto *application = dynamic_cast<Fl_Input *>(find_widget(window, "Executable name"));
  if (!expect(application, "draft executable field")) return;
  application->value("fixture.exe");
  application->do_callback();
  auto *field = dynamic_cast<Fl_Input *>(find_widget(window, "Duration (seconds)"));
  auto *width = dynamic_cast<Fl_Input *>(find_widget(window, "Bar width"));
  auto *height = dynamic_cast<Fl_Input *>(find_widget(window, "Bar height"));
  auto *preview = find_widget(window, "Local panel preview");
  if (!expect(field && width && height && preview, "Windows editor has all four controls")) return;
  if (!switch_to(app, "Cooldowns") || !expect(field->visible_r(), "valid draft can switch to Cooldowns")) return;
  using namespace std::chrono_literals;
  const auto duration = [&] { return app.session.document().profile().cooldowns.front().duration; };
  const auto bar = [&] { return app.session.document().profile().panels.front(); };
  field->take_focus();
  expect(duration() == 5s && handle_event(*field, FL_KEYDOWN, FL_Up) && duration() == 5100ms,
         "Duration Up steps 0.1 seconds through the profile edit");
  expect(handle_event(*field, FL_KEYDOWN, FL_Down, FL_SHIFT) && duration() == 4100ms,
         "Duration Shift+Down steps one second");
  expect(handle_event(*field, FL_MOUSEWHEEL, 0, 0, -1) && duration() == 4200ms,
         "wheel steps the focused Duration");
  app.command(editor::EditorCommand::undo);
  expect(duration() == 4100ms, "each numeric step creates one undo entry");
  field->value("0.1");
  field->do_callback();
  expect(handle_event(*field, FL_KEYDOWN, FL_Down) && duration() == 100ms, "Duration clamps at the lower bound");
  if (!switch_to(app, "Panels") || !expect(width->visible_r(), "valid draft can switch to Panels")) return;
  const auto prior = bar();
  width->take_focus();
  (void)handle_event(*width, FL_KEYDOWN, FL_Right);
  expect(bar().bar.width == prior.bar.width, "horizontal arrows remain text keys in the number field");
  expect(handle_event(*width, FL_KEYDOWN, FL_Up) && bar().bar.width == prior.bar.width + 1,
         "bar width steps one pixel");
  height->take_focus();
  expect(handle_event(*height, FL_MOUSEWHEEL, 0, 0, 1) && bar().bar.height == prior.bar.height - 1,
         "bar height steps one pixel with the wheel");
  height->value(std::to_string(layerc::core::max_bar_height).c_str());
  height->do_callback();
  expect(handle_event(*height, FL_KEYDOWN, FL_Up) && bar().bar.height == layerc::core::max_bar_height,
         "bar height clamps at the upper bound");
  expect(preview->take_focus(), "preview can take keyboard focus on Windows");
  const auto before = bar();
  expect(handle_event(*preview, FL_KEYDOWN, FL_Right) &&
             std::abs(bar().anchor_x - before.anchor_x - 1.0 / preview->w()) < 1e-6,
         "preview Right nudges one target pixel");
  expect(handle_event(*preview, FL_KEYDOWN, FL_Up, FL_SHIFT) &&
             std::abs(bar().anchor_y - before.anchor_y + 10.0 / preview->h()) < 1e-6,
         "preview Shift+Up nudges ten target pixels");
  app.command(editor::EditorCommand::undo);
  expect(std::abs(bar().anchor_x - before.anchor_x - 1.0 / preview->w()) < 1e-6 &&
             bar().anchor_y == before.anchor_y, "only the last nudge is undone");
  auto *anchor = dynamic_cast<Fl_Input *>(find_widget(window, "X (0–1)"));
  if (!expect(anchor, "panel X anchor field")) return;
  anchor->value("1");
  anchor->do_callback();
  expect(handle_event(*preview, FL_KEYDOWN, FL_Right, FL_SHIFT) && bar().anchor_x == 1.0,
         "preview nudge clamps at the target edge");
  window.hide();
}

void show_evidence_tooltip(void *) {
  if (auto *window = Fl::first_window())
    if (auto *widget = find_widget(*window, "&New"))
      Fl_Tooltip::enter_area(widget, 0, 0, widget->w(), widget->h(), widget->tooltip());
}
