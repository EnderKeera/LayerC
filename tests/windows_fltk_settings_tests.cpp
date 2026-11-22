#include "accessible_widgets.hpp"
#include "editor_style.hpp"
#include "options_window.hpp"
#include "platform/win32/settings_store.hpp"
#include "platform/win32/window_settings.hpp"
#include "layerc/platform/window_query.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/platform.H>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace editor = layerc::editor;
namespace platform = layerc::platform;
using layerc::test::expect;

class SettingsFixture {
public:
  SettingsFixture()
      : key_(L"Software\\LayerC.FltkSettingsTests." + std::to_wstring(GetCurrentProcessId()) +
             L"." + std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(++sequence_)),
        settings_("LAYERC_SETTINGS_KEY", std::filesystem::path(key_)),
        profiles_("LAYERC_PROFILE_DIR", directory_.path()) {}
  ~SettingsFixture() {
    const auto removed = RegDeleteTreeW(HKEY_CURRENT_USER, key_.c_str());
    expect(removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND, "remove fixture key");
  }
  [[nodiscard]] bool ready() const { return settings_.applied() && profiles_.applied(); }

private:
  static inline unsigned sequence_{};
  std::wstring key_;
  layerc::test::TempDirectory directory_{"LayerC-FltkSettingsTests"};
  layerc::test::ScopedEnvironment settings_, profiles_;
};

void appearance_mapping() {
  for (const auto theme : {editor::EditorTheme::copper, editor::EditorTheme::teal,
                           editor::EditorTheme::plum, editor::EditorTheme::system,
                           editor::EditorTheme::dark}) {
    editor::EditorAppearance source;
    source.theme = theme;
    for (const auto accent : {std::optional<std::uint32_t>{}, std::optional<std::uint32_t>{0x876543U}}) {
      source.accent = accent;
      for (std::size_t i = 0; i < source.overrides.size(); ++i) {
        const auto n = static_cast<std::uint32_t>(i);
        source.overrides[i] = {.fill = n, .text = n + 10U, .selection = n + 20U};
      }
      const auto native = editor::merge_editor_appearance(source, {});
      expect(native.theme == static_cast<platform::EditorTheme>(theme) &&
                 native.accent == accent.value_or(0), "theme and preset/explicit accent map");
      for (const auto &role : native.overrides)
        expect(role.fields == (platform::editor_fill | platform::editor_text | platform::editor_selection),
               "each editable field maps to the shared role bit");
      expect(editor::to_editor_appearance(native) == source, "appearance round trips for each theme");
    }
  }
  editor::EditorAppearance black;
  black.accent = 0;
  expect(editor::merge_editor_appearance(black, {}).accent == 0x010101U,
         "explicit black does not become the preset accent");
}

void shared_appearance_record() {
  const SettingsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  platform::EditorAppearanceSettings native;
  for (auto &role : native.overrides) {
    role.fields = platform::editor_fill | platform::editor_border | platform::editor_hover |
                  platform::editor_disabled;
    role.fill = 7;
    role.border = 1;
    role.hover = 2;
    role.disabled = 3;
  }
  if (!expect(static_cast<bool>(platform::write_editor_appearance(native)), "seed Win32 appearance")) return;
  editor::EditorAppearance choice;
  choice.theme = editor::EditorTheme::plum;
  choice.overrides.front().selection = 0;
  expect(!editor::save_platform_appearance(choice), "save through the FLTK appearance store");
  const auto saved = platform::read_editor_appearance();
  if (!expect(saved.value.has_value(), "Win32 reads FLTK's save")) return;
  for (std::size_t i = 0; i < saved.value->overrides.size(); ++i) {
    const auto &role = saved.value->overrides[i];
    const auto preserved = platform::editor_border | platform::editor_hover | platform::editor_disabled;
    const auto fields = i == 0 ? preserved | platform::editor_selection : preserved;
    expect(role.fields == fields && role.border == 1 && role.hover == 2 && role.disabled == 3,
           "Win32-only fields and values survive and stale FLTK fields are replaced");
  }
  expect(editor::load_platform_appearance().appearance == choice, "FLTK reads its saved choice");

  constexpr std::array invalid{std::byte{0}};
  expect(static_cast<bool>(platform::write_layerc_registry_binary(L"EditorAppearance", invalid)),
         "seed an invalid structured record");
  for (const auto &loaded : {editor::load_platform_appearance(), editor::load_startup_appearance().editor})
    expect(loaded.appearance == editor::EditorAppearance{} &&
               loaded.error == "The editor appearance settings could not be read; defaults are in use.",
           "invalid record or failed migration reports defaults");
  expect(!editor::save_platform_appearance(choice), "a save can replace the invalid record");
  const auto replaced = platform::read_editor_appearance();
  expect(replaced.value && replaced.value->overrides.front().fields == platform::editor_selection &&
             editor::to_editor_appearance(*replaced.value) == choice,
         "replacement starts from defaults");
}

void startup_migrates_legacy_appearance() {
  const SettingsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  expect(static_cast<bool>(platform::write_layerc_registry_dword(L"EditorTheme", 1)), "seed legacy Teal");
  expect(static_cast<bool>(platform::write_layerc_registry_dword(L"EditorAccent", 0x123456U)),
         "seed legacy accent");
  expect(platform::read_editor_appearance().missing(), "structured record absent before startup");
  const auto loaded = editor::load_startup_appearance();
  expect(loaded.editor.error.empty() && loaded.editor.appearance.theme == editor::EditorTheme::teal &&
             loaded.editor.appearance.accent == 0x123456U,
         "startup migration runs before the first FLTK read");
  expect(platform::read_editor_appearance().value.has_value(), "startup migration persisted a shared record");
}

unsigned reloads{};
void count_reload() { ++reloads; }

void options_store() {
  const SettingsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  expect(editor::editor_options_supported(), "Windows supports interactive Options");
  const auto defaults = editor::load_editor_options(false, editor::load_editor_text_scale());
  expect(defaults.warning.empty() && defaults.options.visual_recognition &&
             defaults.options.overlay_capture_exclusion && !defaults.options.editor_capture_exclusion &&
             !defaults.options.launcher_updates && defaults.options.text_scale == 100,
         "missing settings default to visual/overlays on, editor/updates off, 100 percent");
  reloads = 0;
  expect(!editor::commit_editor_options({false, false, true, true, 115}, count_reload),
         "commit the five settings together");
  using platform::BoolSetting;
  expect(platform::read_bool_setting(BoolSetting::visual_recognition).value == false &&
             platform::read_bool_setting(BoolSetting::capture_exclusion).value == false &&
             platform::read_bool_setting(BoolSetting::editor_capture_exclusion).value == true &&
             platform::read_bool_setting(BoolSetting::launcher_updates).value == true &&
             platform::read_editor_text_scale().value == 115 && reloads == 1,
         "all five read back and the commit requested one reload");
  using platform::SettingsCommitState;
  expect(!editor::options_commit_error(SettingsCommitState::committed), "successful commit has no error text");
  expect(editor::options_commit_error(SettingsCommitState::unchanged) ==
             "Could not save the selected options. Windows refused a setting, so no option was changed. "
             "Choose Apply to try again.", "unchanged failure matches the Win32 warning");
  expect(editor::options_commit_error(SettingsCommitState::partial) ==
             "Could not save the selected options. Windows refused a setting, and some options it had "
             "already saved could not be restored. Choose Apply again to save them all.",
         "partial failure matches the Win32 warning");
}

Fl_Widget *child(Fl_Group &group, const std::string_view label) {
  for (int i = 0; i < group.children(); ++i) {
    auto *widget = group.child(i);
    if (widget->label() && label == widget->label()) return widget;
  }
  return nullptr;
}

void options_dialog_actions_and_descriptions() {
  const SettingsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  unsigned commits = 0, applied = 0, failures = 0;
  bool refuse = true;
  editor::OptionsWindow dialog({}, {}, 640,
      [&](const editor::EditorOptions &options) -> std::optional<std::string_view> {
        ++commits;
        if (refuse) return editor::options_commit_error(platform::SettingsCommitState::unchanged);
        return editor::commit_editor_options(options, count_reload);
      }, [&](const editor::EditorOptions &) { ++applied; },
         [&](std::string_view) { ++failures; });
  constexpr std::array hints{
      std::pair{"Enable &visual recognition", "Watches configured screen regions only while a profile needs visual matching."},
      std::pair{"Hide cooldown &overlays from screen capture and streams", "Screenshots, screen sharing and Magnifier cannot see a hidden window."},
      std::pair{"Hide the &editor from screen capture", "Screenshots, screen sharing and Magnifier cannot see a hidden window."},
      std::pair{"&Text size", "Adds to the Windows text size setting."},
      std::pair{"Install &updates when LayerC starts", "The launcher checks GitHub for a signed release before it starts LayerD."},
  };
  for (const auto &[label, hint] : hints) {
    const auto *widget = child(dialog, label);
    expect(widget && editor::accessibility::accessible_description(*widget) == hint,
           "each hint describes its control in the accessibility tree");
  }
  auto *visual = dynamic_cast<Fl_Check_Button *>(child(dialog, hints.front().first));
  auto *apply = child(dialog, "&Apply");
  auto *ok = child(dialog, "OK");
  auto *cancel = child(dialog, "Cancel");
  if (!expect(visual && apply && ok && cancel, "dialog has the check box and all actions")) return;
  expect(!apply->active(), "Apply starts disabled");
  visual->value(0);
  visual->do_callback();
  expect(apply->active(), "changing a row enables Apply");
  dialog.show();
  ok->do_callback();
  expect(dialog.shown() && apply->active() && failures == 1 && applied == 0,
         "OK stays open and Apply stays enabled when the commit fails");
  refuse = false;
  apply->do_callback();
  expect(dialog.shown() && !apply->active() && commits == 2 && applied == 1 &&
             platform::read_bool_setting(platform::BoolSetting::visual_recognition).value == false,
         "Apply commits and disables itself without closing");
  ok->do_callback();
  expect(!dialog.shown() && commits == 2, "OK with no changes closes without another write");
  dialog.show();
  visual->value(1);
  visual->do_callback();
  cancel->do_callback();
  expect(!dialog.shown() && commits == 2 &&
             platform::read_bool_setting(platform::BoolSetting::visual_recognition).value == false,
         "Cancel closes without writing the edited row");
}

bool has_affinity(Fl_Window &window, const DWORD expected) {
  DWORD actual{};
  return GetWindowDisplayAffinity(fl_xid(&window), &actual) != FALSE && actual == expected;
}

bool wait_for_affinity(Fl_Window &window, const DWORD expected) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  do {
    Fl::check();
    if (has_affinity(window, expected)) return true;
    Sleep(1);
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

void capture_exclusion_on_existing_and_new_windows() {
  const SettingsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  Fl_Double_Window first(220, 100, "Settings capture fixture");
  first.end();
  first.show();
  editor::set_editor_capture_exclusion(true);
  expect(has_affinity(first, platform::capture_exclusion_affinity), "turning on immediately excludes a shown window");
  Fl_Double_Window later(220, 100, "Later capture fixture");
  later.end();
  later.show();
  expect(wait_for_affinity(later, platform::capture_exclusion_affinity), "first paint excludes a later window");
  editor::set_editor_capture_exclusion(false);
  expect(has_affinity(first, WDA_NONE) && has_affinity(later, WDA_NONE), "turning off clears both affinities");
  Fl_Double_Window last(220, 100, "Capturable fixture");
  last.end();
  last.show();
  last.wait_for_expose();
  Fl::check();
  expect(!editor::editor_capture_exclusion_enabled() && has_affinity(last, WDA_NONE),
         "no capture handler applies affinity after exclusion is turned off");
}

int shortcut(Fl_Window &window, const int key, const int modifiers, char *text) {
  const auto prior_key = Fl::e_keysym;
  const auto prior_state = Fl::e_state;
  auto *const prior_text = Fl::e_text;
  const auto prior_length = Fl::e_length;
  Fl::e_keysym = key;
  Fl::e_state = modifiers;
  Fl::e_text = text;
  Fl::e_length = 1;
  const int handled = Fl::handle(FL_SHORTCUT, &window);
  Fl::e_keysym = prior_key;
  Fl::e_state = prior_state;
  Fl::e_text = prior_text;
  Fl::e_length = prior_length;
  return handled;
}

void text_scale_startup_and_keys() {
  const SettingsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  std::vector<float> baseline;
  for (int i = 0; i < Fl::screen_count(); ++i) baseline.push_back(Fl::screen_scale(i));
  const auto matches_scale = [&](const float ratio) {
    for (int i = 0; i < Fl::screen_count(); ++i)
      if (std::abs(Fl::screen_scale(i) - baseline[static_cast<std::size_t>(i)] * ratio) >= 1e-3F)
        return false;
    return true;
  };
  expect(static_cast<bool>(platform::write_editor_text_scale(130)), "seed a stored text size");
  editor::load_startup_text_scale();
  expect(editor::current_editor_text_scale() == 130 && matches_scale(1.3F),
         "startup scales all monitors before showing a window");

  Fl_Double_Window window(220, 100, "Text size fixture");
  window.end();
  window.show();
  std::string status;
  char plus[] = "+", minus[] = "-", zero[] = "0", equals[] = "=";
  {
    const editor::WindowsTextScaleShortcuts keys([&](editor::StatusKind, std::string text) { status = text; });
    expect(static_cast<bool>(platform::write_editor_text_scale(115)), "seed a sentinel for a clamped write");
    expect(shortcut(window, '=', FL_CTRL | FL_SHIFT, plus) &&
               platform::read_editor_text_scale().value == 115 && matches_scale(1.3F) && status.empty(),
           "Ctrl+Plus at the upper end is consumed without a write or another zoom");
    expect(shortcut(window, '0', FL_CTRL, zero) &&
               platform::read_editor_text_scale().value == 100 && matches_scale(1.0F) &&
               status == "Text size 100 %.", "Ctrl+0 persists 100 and restores the full baseline");
    expect(static_cast<bool>(platform::write_editor_text_scale(115)), "seed a second sentinel");
    expect(shortcut(window, '-', FL_CTRL, minus) &&
               platform::read_editor_text_scale().value == 115 && matches_scale(1.0F),
           "Ctrl+Minus at the lower end is consumed without a write");
    expect(shortcut(window, '=', FL_CTRL | FL_SHIFT, plus) &&
               platform::read_editor_text_scale().value == 115 && matches_scale(1.15F) &&
               status == "Text size 115 %.", "Ctrl+Plus uses the persistent discrete scale, not FLTK zoom");
    const bool simple = Fl::option(Fl::OPTION_SIMPLE_ZOOM_SHORTCUT);
    Fl::option(Fl::OPTION_SIMPLE_ZOOM_SHORTCUT, true);
    expect(shortcut(window, '=', FL_CTRL, equals) &&
               platform::read_editor_text_scale().value == 130 && matches_scale(1.3F),
           "Ctrl+= advances when the FLTK simple zoom option is on");
    Fl::option(Fl::OPTION_SIMPLE_ZOOM_SHORTCUT, simple);
  }
  expect(!shortcut(window, '0', FL_CTRL, zero) && matches_scale(1.3F),
         "the owned shortcut handler is gone and FLTK's own zoom handler is not active");
  expect(static_cast<bool>(platform::write_layerc_registry_dword(L"EditorTextScale", 123)),
         "seed an unknown stored text size");
  editor::load_startup_text_scale();
  expect(editor::current_editor_text_scale() == 100 && matches_scale(1.0F),
         "an unsupported stored text size normalizes to 100 percent");
}

} // namespace

int main() {
  Fl::keyboard_screen_scaling(0);
  fl_open_display();
  return layerc::test::run({
      LAYERC_TEST_CASE(appearance_mapping),
      LAYERC_TEST_CASE(shared_appearance_record),
      LAYERC_TEST_CASE(startup_migrates_legacy_appearance),
      LAYERC_TEST_CASE(options_store),
      LAYERC_TEST_CASE(options_dialog_actions_and_descriptions),
      LAYERC_TEST_CASE(capture_exclusion_on_existing_and_new_windows),
      LAYERC_TEST_CASE(text_scale_startup_and_keys),
  });
}
