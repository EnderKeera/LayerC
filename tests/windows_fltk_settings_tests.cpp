#include "accessible_widgets.hpp"
#include "appearance_window.hpp"
#include "editor_color.hpp"
#include "editor_style.hpp"
#include "linux_editor_test_access.hpp"
#include "options_window.hpp"
#include "platform/contrast.hpp"
#include "platform/win32/desktop_listener.hpp"
#include "platform/win32/settings_store.hpp"
#include "platform/win32/startup.hpp"
#include "platform/win32/window_chrome.hpp"
#include "platform/win32/window_settings.hpp"
#include "layerc/platform/native_theme.hpp"
#include "layerc/platform/window_query.hpp"
#include "test_support.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Tooltip.H>
#include <FL/platform.H>
#include <dwmapi.h>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

void editor_tooltips_keys_and_menu();
void stepped_fields_and_preview_nudge();
void show_evidence_tooltip(void *);

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
    role.disabled = 0x808080U;
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
    expect(role.fields == fields && role.border == 1 && role.hover == 2 && role.disabled == 0x808080U,
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

template <class Predicate> bool wait_for(Predicate ready) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  do {
    Fl::check();
    if (ready()) return true;
    Sleep(1);
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

void capture_exclusion_on_existing_and_new_windows() {
  const SettingsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  editor::WindowsWindowDecorator decorator;
  Fl_Double_Window first(220, 100, "Settings capture fixture");
  first.end();
  first.show();
  editor::set_editor_capture_exclusion(true);
  expect(has_affinity(first, platform::capture_exclusion_affinity), "turning on immediately excludes a shown window");
  Fl_Double_Window later(220, 100, "Later capture fixture");
  later.end();
  later.show();
  expect(wait_for([&] { return has_affinity(later, platform::capture_exclusion_affinity); }),
         "first paint excludes a later window");
  editor::set_editor_capture_exclusion(false);
  expect(has_affinity(first, WDA_NONE) && has_affinity(later, WDA_NONE), "turning off clears both affinities");
  Fl_Double_Window last(220, 100, "Capturable fixture");
  last.end();
  last.show();
  last.wait_for_expose();
  Fl::check();
  expect(!editor::editor_capture_exclusion_requested() && has_affinity(last, WDA_NONE),
         "the decorator does not exclude new windows after capture exclusion is turned off");
}

struct AffinityProbe {
  enum class Result { allow, deny, unchanged, deny_clear };
  static inline HWND window{};
  static inline Result result{Result::allow};

  static bool set(HWND target, bool exclude) {
    if (target == window) {
      if (exclude && result == Result::deny) return false;
      if (exclude && result == Result::unchanged) return true;
      if (!exclude && result == Result::deny_clear) return false;
    }
    return platform::apply_capture_exclusion(target, exclude);
  }
};

void capture_exclusion_failure_and_recovery() {
  const SettingsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  AffinityProbe::window = nullptr;
  AffinityProbe::result = AffinityProbe::Result::allow;
  editor::EditorApp app;
  editor::WindowsWindowDecorator decorator(AffinityProbe::set);
  app.build();
  app.show();
  auto *const status = editor::EditorAppTestAccess::status(app);
  decorator.set_warning_reporter([&](std::string_view warning) { app.set_capture_exclusion_warning(warning); });
  editor::OptionsWindow dialog({}, app.appearance(), 640,
      [](const editor::EditorOptions &options) { return editor::commit_editor_options(options, count_reload); },
      [&](const editor::EditorOptions &options) {
        editor::apply_editor_options_live(options);
        app.report(editor::StatusKind::success, "Options saved.");
      }, [](std::string_view) { expect(false, "the settings write succeeds"); });
  auto *const capture = dynamic_cast<Fl_Check_Button *>(child(dialog, "Hide the &editor from screen capture"));
  auto *const visual = dynamic_cast<Fl_Check_Button *>(child(dialog, "Enable &visual recognition"));
  auto *const apply = child(dialog, "&Apply");
  if (!expect(status && capture && visual && apply, "Options fixture exposes settings and status")) return;
  const auto warning = [] { return editor::editor_capture_exclusion_warning(); };
  using platform::BoolSetting;
  auto &main = *app.main_window();
  dialog.show();
  AffinityProbe::window = fl_xid(&dialog);
  AffinityProbe::result = AffinityProbe::Result::deny;
  capture->value(1);
  capture->do_callback();
  visual->value(0);
  visual->do_callback();
  apply->do_callback();
  expect(editor::editor_capture_exclusion_requested() &&
             platform::read_bool_setting(BoolSetting::editor_capture_exclusion).value == true &&
             platform::read_bool_setting(BoolSetting::visual_recognition).value == false,
         "Options persists both preferences despite failing to hide one window");
  expect(has_affinity(main, platform::capture_exclusion_affinity) && has_affinity(dialog, WDA_NONE) &&
             warning() == editor::capture_exclusion_incomplete,
         "successful affinity on the main window does not hide the dialog's refusal");
  const auto visible_warning = [&](std::string_view text) {
    const std::string hint = "Screenshots, screen sharing and Magnifier cannot see a hidden window.\n" +
                             std::string(text);
    return child(dialog, hint) && editor::accessibility::accessible_description(*capture) == hint;
  };
  expect(visible_warning(warning()) && std::string_view(status->value()).starts_with(warning()),
         "refusal is rendered in Options and the main status, and describes the setting");
  app.report(editor::StatusKind::information, "Unrelated outcome.");
  expect(std::string_view(status->value()).starts_with(warning()) &&
             std::string_view(status->value()).ends_with("Unrelated outcome."),
         "unrelated action outcomes do not clear the outstanding privacy warning");

  AffinityProbe::result = AffinityProbe::Result::unchanged;
  decorator.refresh();
  expect(warning() == editor::capture_exclusion_incomplete,
         "an apparent setter success is not enough without matching readback");
  AffinityProbe::result = AffinityProbe::Result::allow;
  InvalidateRect(AffinityProbe::window, nullptr, FALSE);
  expect(wait_for([&] { return warning().empty() && has_affinity(dialog, platform::capture_exclusion_affinity); }),
         "a later paint retries previously failed affinity without changing the preference");
  dialog.refresh_capture_warning();
  expect(std::string_view(status->value()) == "Unrelated outcome." &&
             !visible_warning(editor::capture_exclusion_incomplete) &&
             editor::accessibility::accessible_description(*capture) ==
                 "Screenshots, screen sharing and Magnifier cannot see a hidden window.",
         "verified recovery clears the fault and restores the capture hint");

  AffinityProbe::result = AffinityProbe::Result::deny_clear;
  capture->value(0);
  capture->do_callback();
  apply->do_callback();
  expect(!editor::editor_capture_exclusion_requested() && has_affinity(main, WDA_NONE) &&
             has_affinity(dialog, platform::capture_exclusion_affinity) &&
             warning() == editor::capture_exclusion_not_cleared && visible_warning(warning()),
         "clearing the preference reports failure until every managed window is capturable");
  AffinityProbe::result = AffinityProbe::Result::allow;
  editor::set_editor_capture_exclusion(false);
  expect(warning().empty() && has_affinity(dialog, WDA_NONE), "an explicit retry verifies clearing affinity");
  dialog.hide();

  editor::set_editor_capture_exclusion(true);
  Fl_Double_Window later(220, 100, "Later capture refusal");
  later.end();
  later.show();
  AffinityProbe::window = fl_xid(&later);
  AffinityProbe::result = AffinityProbe::Result::deny;
  expect(wait_for([&] { return warning() == editor::capture_exclusion_incomplete; }) &&
             std::string_view(status->value()).starts_with(warning()),
         "a newly shown unprotected window reports a fault without reapplying Options");
  later.hide();
  expect(wait_for([&] { return warning().empty(); }) && has_affinity(main, platform::capture_exclusion_affinity),
         "closing the unprotected window clears its fault without unprotecting the main window");
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

struct ContrastOverride {
  ~ContrastOverride() { platform::set_native_high_contrast_override(std::nullopt); }
};

std::uint32_t system_rgb(const int index) { return platform::native_rgb_value(GetSysColor(index)); }

void contrast_mapping_and_desktop_listener() {
  const SettingsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  const ContrastOverride reset;
  platform::set_native_high_contrast_override(false);
  expect(!editor::platform_contrast_palette(), "contrast forced off supplies no override");
  editor::EditorAppearance appearance;
  appearance.theme = editor::EditorTheme::plum;
  const auto role = static_cast<std::size_t>(editor::EditorRole::window);
  const auto button = static_cast<std::size_t>(editor::EditorRole::button);
  appearance.overrides[role].fill = editor::theme_role_colors(appearance, editor::EditorRole::window)->text;
  const auto saved = appearance;
  Fl_Double_Window window(220, 100, "Contrast fixture");
  window.end();
  window.show();
  editor::apply_editor_appearance(appearance, window);
  const auto normal_tokens = editor::current_editor_tokens();
  unsigned reapplies{};
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const auto reapply = [&] {
    ++reapplies;
    editor::reapply_editor_desktop_appearance(window);
  };
  std::optional<editor::WindowsDesktopListener> registrar(std::in_place, instance, reapply);
  std::optional<editor::WindowsDesktopListener> second(std::in_place, instance, reapply);
  const HWND target = registrar->native_handle();
  const HWND surviving = second->native_handle();
  if (!expect(surviving && surviving != target,
              "another listener creates a separate window using the same class")) return;
  if (!expect(target && !IsWindowVisible(target) && !GetWindow(target, GW_OWNER) &&
                  (GetWindowLongPtrW(target, GWL_STYLE) & WS_POPUP) &&
                  (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOOLWINDOW),
              "desktop listener is an invisible, unowned top-level tool window")) return;
  platform::set_native_high_contrast_override(true);
  const auto palette = editor::platform_contrast_palette();
  if (!expect(palette.has_value(), "contrast forced on supplies the palette")) return;
  const auto fill = system_rgb(COLOR_WINDOW), text = system_rgb(COLOR_WINDOWTEXT);
  const auto face = system_rgb(COLOR_BTNFACE), button_text = system_rgb(COLOR_BTNTEXT);
  const auto gray = system_rgb(COLOR_GRAYTEXT), selection = system_rgb(COLOR_HIGHLIGHT);
  const auto on_selection = system_rgb(COLOR_HIGHLIGHTTEXT);
  for (std::size_t i = 0; i < palette->roles.size(); ++i) {
    const editor::RoleColors expected{i == button ? face : fill,
                                      i == button ? button_text : text, selection};
    expect(palette->roles[i] == expected, "each contrast role maps to its Windows system colors");
  }
  const editor::EditorTokens tokens{
      .background = fill, .panel = face, .input = fill, .border = text, .divider = gray,
      .text = text, .secondary = text, .subtext = text, .verbose = text, .disabled = gray,
      .button_hover = face, .row_hover = fill, .pressed = face, .accent = selection,
      .selection = selection, .on_accent = on_selection, .yes = text, .no = text,
      .warning = text, .accent_2 = text, .on_status = fill};
  expect(palette->tokens == tokens, "every contrast token maps to the DesignSystem table");
  Fl_Double_Window other(220, 100, "Second contrast fixture");
  other.end();
  other.show();
  SendMessageW(target, WM_SYSCOLORCHANGE, 0, 0);
  SendMessageW(target, WM_SETTINGCHANGE, SPI_SETHIGHCONTRAST, 0);
  SendMessageW(target, WM_THEMECHANGED, 0, 0);
  expect(!reapplies, "sent messages never repaint synchronously");
  expect(wait_for([&] { return reapplies == 1; }), "desktop notifications coalesce into one reapply");
  expect(editor::current_editor_tokens() == tokens && Fl::get_color(window.color()) == (fill << 8U) &&
             Fl::get_color(other.color()) == (fill << 8U) &&
             Fl::get_color(FL_FOREGROUND_COLOR) == (text << 8U) &&
             Fl::get_color(Fl_Tooltip::color()) == (fill << 8U) &&
             Fl::get_color(Fl_Tooltip::textcolor()) == (text << 8U) &&
             Fl_Tooltip::size() == FL_NORMAL_SIZE && Fl_Tooltip::wrap_width() == 360,
         "all shown windows, global palette, draw tokens and tooltips switch together");
  platform::set_native_high_contrast_override(false);
  SendMessageW(target, WM_SETTINGCHANGE, SPI_SETHIGHCONTRAST, 0);
  expect(wait_for([&] { return reapplies == 2; }) && editor::current_editor_tokens() == normal_tokens &&
             appearance == saved && Fl::get_color(window.color()) == (*saved.overrides[role].fill << 8U),
         "disabling contrast restores the preset and retained override");

  constexpr wchar_t listener_class[] = L"LayerC.LayerE.FLTK.Desktop";
  WNDCLASSW definition{};
  registrar.reset();
  expect(IsWindow(target) == FALSE && IsWindow(surviving) != FALSE &&
             GetClassInfoW(instance, listener_class, &definition) != FALSE,
         "destroying the registrar leaves the other listener and its class alive");
  const auto before = reapplies;
  SendMessageW(surviving, WM_SYSCOLORCHANGE, 0, 0);
  expect(wait_for([&] { return reapplies == before + 1; }),
         "the last surviving listener still reapplies on a system color change");
  second.reset();
  expect(GetClassInfoW(instance, listener_class, &definition) == FALSE,
         "destroying the last listener unregisters the class");
  const editor::WindowsDesktopListener fresh(instance, [] {});
  expect(fresh.native_handle() && IsWindow(fresh.native_handle()) != FALSE,
         "a new listener registers the class and creates a window again");
}

// DesignSystem.md, Roles: a contrast theme sets the overrides aside, so an
// unreadable override neither warns nor refuses while it is on.
void appearance_contrast_theme_sets_floors_aside() {
  const SettingsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  const ContrastOverride reset;
  platform::set_native_high_contrast_override(true);
  Fl_Double_Window root(220, 100, "Appearance contrast fixture");
  root.end();
  root.show();
  struct Drive {
    bool delivered{}, quiet{};
    int attempts{};
  } drive;
  struct Step {
    static void tick(void *data) {
      auto &state = *static_cast<Drive *>(data);
      auto *modal = Fl::modal();
      if (!modal) {
        if (++state.attempts < 200) Fl::repeat_timeout(0.01, &Step::tick, data);
        return;
      }
      state.delivered = true;
      auto *role = dynamic_cast<Fl_Choice *>(child(*modal, "Role"));
      auto *text = dynamic_cast<Fl_Input *>(child(*modal, "Role text"));
      auto *apply = child(*modal, "Apply");
      if (!role || !text || !apply) { modal->hide(); return; }
      role->value(static_cast<int>(editor::EditorRole::list));
      role->do_callback();
      text->value("#000000");
      text->do_callback();
      bool warning_mark{};
      std::string line;
      for (int i = 0; i < modal->children(); ++i) {
        if (auto *mark = dynamic_cast<editor::EditorStatusMark *>(modal->child(i))) {
          warning_mark = mark->kind() != editor::StatusKind::none;
          if (i + 1 < modal->children() && modal->child(i + 1)->label())
            line = modal->child(i + 1)->label();
        }
      }
      state.quiet = !warning_mark && line == "A contrast theme is on, so Windows colors are in use.";
      apply->do_callback();
      if (modal->shown()) modal->hide();
    }
  };
  Fl::add_timeout(0.01, &Step::tick, &drive);
  const auto result = editor::edit_editor_appearance({}, root, nullptr, nullptr);
  Fl::remove_timeout(&Step::tick, &drive);
  expect(drive.delivered && drive.quiet,
         "the contrast notice replaces the warning line and shows no warning glyph");
  expect(result && result->overrides[static_cast<std::size_t>(editor::EditorRole::list)].text == 0U,
         "Apply keeps the set-aside override while a contrast theme is on");
  platform::set_native_high_contrast_override(false);
  editor::apply_editor_appearance({}, root);
}

// Runs `action` on the Appearance dialog once FLTK has mapped it.
std::optional<editor::EditorAppearance>
drive_appearance(Fl_Window &root, std::function<void(Fl_Window &)> action, bool &delivered) {
  struct Drive {
    std::function<void(Fl_Window &)> action;
    bool *delivered{};
    int attempts{};
    static void tick(void *data) {
      auto &self = *static_cast<Drive *>(data);
      if (auto *modal = Fl::modal()) {
        *self.delivered = true;
        self.action(*modal);
        if (modal->shown()) modal->hide();
        return;
      }
      if (++self.attempts < 200) Fl::repeat_timeout(0.01, &Drive::tick, data);
    }
  } drive{std::move(action), &delivered};
  delivered = false;
  Fl::add_timeout(0.01, &Drive::tick, &drive);
  auto result = editor::edit_editor_appearance({}, root, nullptr, nullptr);
  Fl::remove_timeout(&Drive::tick, &drive);
  return result;
}

// The warning line after the dialog's status mark, and whether it warns.
std::pair<std::string, bool> appearance_line(Fl_Window &modal) {
  for (int i = 0; i < modal.children(); ++i)
    if (auto *mark = dynamic_cast<editor::EditorStatusMark *>(modal.child(i))) {
      const auto *line = i + 1 < modal.children() ? modal.child(i + 1) : nullptr;
      return {line && line->label() ? line->label() : "",
              mark->kind() == editor::StatusKind::warning};
    }
  return {"<no mark>", false};
}

// review layer55 F-01: FLTK's Appearance measures the shared record the
// Win32 editor paints, with the hover part FLTK keeps but cannot edit.
void appearance_shared_record_floors() {
  const SettingsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  const ContrastOverride reset;
  platform::set_native_high_contrast_override(false);
  constexpr auto buttons = static_cast<std::size_t>(editor::EditorRole::button);
  const auto seed = [](const std::uint32_t hover) {
    platform::EditorAppearanceSettings native;
    native.overrides[buttons].fields = platform::editor_hover;
    native.overrides[buttons].hover = hover;
    return static_cast<bool>(platform::write_editor_appearance(native));
  };
  editor::EditorAppearance white_text;
  white_text.overrides[buttons].text = 0xFFFFFFU;
  expect(!editor::first_role_contrast_failure(white_text, {}),
         "white Buttons text clears FLTK's own floors");
  if (!expect(seed(0x101010U), "seed a dark retained Buttons hover")) return;
  expect(!editor::shared_appearance_failure(white_text),
         "white text reads on the dark hover the Win32 editor paints");
  if (!expect(seed(0xFFFFFFU), "seed a white retained Buttons hover")) return;
  const std::string refusal =
      "Buttons: Text / Hover contrast is 1.0:1; at least 4.5:1 is required.";
  const auto failure = editor::shared_appearance_failure(white_text);
  expect(failure && failure->role == editor::EditorRole::button &&
             failure->pair == editor::ContrastPairKind::text_hover && !failure->ground &&
             editor::role_contrast_failure_text(*failure) == refusal,
         "white text on the retained white hover refuses in the Win32 editor's words");

  Fl_Double_Window root(220, 100, "Appearance shared record fixture");
  root.end();
  root.show();
  bool delivered{}, warned{}, described{}, refused{};
  const auto refused_result = drive_appearance(root, [&](Fl_Window &modal) {
    auto *role = dynamic_cast<Fl_Choice *>(child(modal, "Role"));
    auto *text = dynamic_cast<Fl_Input *>(child(modal, "Role text"));
    auto *apply = child(modal, "Apply");
    if (!role || !text || !apply) return;
    role->value(static_cast<int>(buttons));
    role->do_callback();
    text->value("#FFFFFF");
    text->do_callback();
    const auto [line, warning] = appearance_line(modal);
    warned = line == refusal && warning;
    described = editor::accessibility::accessible_description(*text) == refusal;
    role->value(0);
    role->do_callback();
    apply->do_callback();
    refused = modal.shown() && role->value() == static_cast<int>(buttons) &&
              Fl::focus() == text && appearance_line(modal).first == refusal;
  }, delivered);
  expect(delivered && warned, "the dialog warns with the shared record's refusal");
  expect(described, "the role text field is described by the warning line");
  expect(refused && !refused_result,
         "Apply refuses, selects Buttons and focuses its text; the dialog is cancelled");
  const auto kept = platform::read_editor_appearance();
  expect(kept.value && kept.value->overrides[buttons].fields == platform::editor_hover &&
             kept.value->overrides[buttons].hover == 0xFFFFFFU,
         "the refused draft leaves the shared record unchanged");

  // A contrast theme sets every floor aside, the shared record's included.
  platform::set_native_high_contrast_override(true);
  bool quiet{};
  const auto contrast_result = drive_appearance(root, [&](Fl_Window &modal) {
    auto *role = dynamic_cast<Fl_Choice *>(child(modal, "Role"));
    auto *text = dynamic_cast<Fl_Input *>(child(modal, "Role text"));
    auto *apply = child(modal, "Apply");
    if (!role || !text || !apply) return;
    role->value(static_cast<int>(buttons));
    role->do_callback();
    text->value("#FFFFFF");
    text->do_callback();
    const auto [line, warning] = appearance_line(modal);
    quiet = !warning && line == "A contrast theme is on, so Windows colors are in use.";
    apply->do_callback();
  }, delivered);
  expect(delivered && quiet, "a contrast theme shows its notice instead of the shared refusal");
  expect(contrast_result && contrast_result->overrides[buttons].text == 0xFFFFFFU,
         "Apply keeps the draft while a contrast theme is on");
  platform::set_native_high_contrast_override(false);
  editor::apply_editor_appearance({}, root);
}

void appearance_primary_button_and_system_floors() {
  const SettingsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  const ContrastOverride reset;
  platform::set_native_high_contrast_override(false);
  editor::install_editor_style();
  Fl_Double_Window root(260, 160, "Appearance primary fixture");
  root.end();
  root.show();

  const auto ratio = [](Fl_Color a, Fl_Color b) {
    return editor::rgb_contrast_ratio(Fl::get_color(a) >> 8U, Fl::get_color(b) >> 8U);
  };
  bool delivered{};
  const auto fixed = drive_appearance(root, [&](Fl_Window &modal) {
    auto *accent = dynamic_cast<Fl_Input *>(child(modal, "Accent #RRGGBB"));
    auto *apply = dynamic_cast<Fl_Button *>(child(modal, "Apply"));
    if (!expect(accent && apply, "Appearance owns the accent field and primary Apply")) return;
    accent->value("#888888");
    accent->do_callback();
    const auto [line, warning] = appearance_line(modal);
    expect(ratio(apply->labelcolor(), apply->color()) < 4.5 && warning &&
               line.find("on the primary button accent") != std::string::npos &&
               editor::accessibility::accessible_description(*accent) == line,
           "actual 3.54:1 primary ink triggers the visible and accessible refusal");
    apply->do_callback();
    expect(modal.shown() && Fl::focus() == accent &&
               platform::read_editor_appearance().missing(),
           "refusing an unreadable accent focuses its field and leaves the registry alone");
    accent->value("#909090");
    accent->do_callback();
    const auto [pressed_line, pressed_warning] = appearance_line(modal);
    expect(ratio(apply->labelcolor(), apply->color()) >= 4.5 &&
               ratio(apply->labelcolor(), apply->selection_color()) < 4.5 &&
               pressed_warning && pressed_line.find("primary button pressed fill") != std::string::npos,
           "a readable idle color can still refuse the pressed button pair");
    accent->value("");
    accent->do_callback();
    expect(appearance_line(modal).first.empty() &&
               ratio(apply->labelcolor(), apply->color()) >= 4.5 &&
               ratio(apply->labelcolor(), apply->selection_color()) >= 4.5 &&
               ratio(fl_contrast(apply->labelcolor(), apply->selection_color()),
                     apply->selection_color()) >= 4.5,
           "the restored preset paints readable idle and both pressed inks");
    apply->do_callback();
  }, delivered);
  expect(delivered && fixed == editor::EditorAppearance{}, "reset accent allows Apply");

  // Every published preset passes the primary floors in both editors. System
  // follows the Windows accent, which this fixture does not control.
  bool presets_readable{};
  (void)drive_appearance(root, [&](Fl_Window &modal) {
    auto *theme = dynamic_cast<Fl_Choice *>(child(modal, "Theme"));
    auto *apply = dynamic_cast<Fl_Button *>(child(modal, "Apply"));
    auto *cancel = child(modal, "Cancel");
    if (!expect(theme && apply && cancel, "Appearance owns Theme, Apply and Cancel")) return;
    presets_readable = true;
    for (int index = 0; index < static_cast<int>(editor::EditorTheme::count); ++index) {
      theme->value(index);
      theme->do_callback();
      editor::EditorAppearance preset;
      preset.theme = static_cast<editor::EditorTheme>(index);
      const bool system_theme = preset.theme == editor::EditorTheme::system;
      const auto pressed = apply->selection_color();
      const bool readable =
          appearance_line(modal).first.empty() && !editor::shared_appearance_failure(preset) &&
          (system_theme || (ratio(apply->labelcolor(), apply->color()) >= 4.5 &&
                            ratio(apply->labelcolor(), pressed) >= 4.5 &&
                            ratio(fl_contrast(apply->labelcolor(), pressed), pressed) >= 4.5));
      const std::string name = theme->text() ? theme->text() : "?";
      presets_readable = expect(readable, (name + " clears the primary floors").c_str()) &&
                         presets_readable;
    }
    cancel->do_callback();
  }, delivered);
  expect(delivered && presets_readable, "no published preset refuses its own primary Apply");

  editor::EditorAppearance system;
  system.theme = editor::EditorTheme::system;
  expect(!editor::shared_appearance_failure(system), "untouched System colors never refuse");
  system.accent = 0x888888U;
  const auto native_accent = editor::shared_appearance_failure(system);
  expect(native_accent && native_accent->primary_button &&
             native_accent->pair == editor::ContrastPairKind::text_fill,
         "an explicit System accent checks the same native primary pair");
  system.accent.reset();
  expect(!editor::save_platform_appearance(system), "save the untouched System palette");

  constexpr auto buttons = static_cast<std::size_t>(editor::EditorRole::button);
  platform::EditorAppearanceSettings later;
  later.theme = platform::EditorTheme::system;
  later.accent = 0xD88E4BU;
  later.overrides.front().fields = platform::editor_border;
  later.overrides[buttons].fields = platform::editor_hover;
  later.overrides[buttons].hover = 0xF3F3F3U;
  expect(static_cast<bool>(platform::write_editor_appearance(later)),
         "a later registry record carries an unreadable Win32-only part");
  const auto hover = editor::shared_appearance_failure(system);
  expect(hover && hover->role == editor::EditorRole::button &&
             hover->pair == editor::ContrastPairKind::text_hover && !hover->primary_button,
         "a retained System hover checks its pair even with an unrelated border override");
  const auto refused = editor::save_platform_appearance(system);
  const auto kept = platform::read_editor_appearance();
  expect(refused && refused->find("Buttons: Text / Hover contrast is 1.0:1") != std::string::npos &&
             kept.value && kept.value->accent == later.accent &&
             kept.value->overrides[buttons].hover == 0xF3F3F3U,
         "persistence rechecks the merged record without replacing it on refusal");
  editor::apply_editor_appearance({}, root);
}

void caption_decoration_and_pruning() {
  const SettingsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  const ContrastOverride reset;
  platform::set_native_high_contrast_override(false);
  editor::WindowsWindowDecorator decorator;
  Fl_Double_Window first(220, 100, "Caption fixture");
  first.end();
  first.show();
  editor::EditorAppearance dark;
  dark.theme = editor::EditorTheme::dark;
  editor::apply_editor_appearance(dark, first);
  const auto check_frame = [](Fl_Window &window) {
    const auto toolbar = editor::current_editor_toolbar_colors();
    const std::array attributes{35UL, 34UL, 36UL}; // Caption, border, text (Windows 11).
    const std::array expected{platform::native_color_ref(toolbar.fill),
        platform::native_color_ref(editor::current_editor_tokens().divider),
        platform::native_color_ref(toolbar.text)};
    for (std::size_t i = 0; i < attributes.size(); ++i) {
      COLORREF actual{};
      const auto result = DwmGetWindowAttribute(fl_xid(&window), attributes[i], &actual, sizeof(actual));
      if (result == E_INVALIDARG || result == E_NOTIMPL) {
        std::puts("[skip] DWM does not read caption colours back on this Windows version.");
        return;
      }
      expect(SUCCEEDED(result) && actual == expected[i], "DWM reads back the current role color");
    }
  };
  expect(decorator.decorated_window_count() == 1, "appearance refresh decorates the shown window");
  check_frame(first);
  Fl_Double_Window later(220, 100, "Later caption fixture");
  later.end();
  later.show();
  expect(wait_for([&] { return decorator.decorated_window_count() == 2; }),
         "first paint decorates a later captioned window");
  check_frame(later);
  platform::set_native_high_contrast_override(true);
  editor::reapply_editor_desktop_appearance(first);
  check_frame(first);
  check_frame(later);
  const HWND old = fl_xid(&later);
  later.hide();
  decorator.refresh();
  expect(!IsWindow(old) && decorator.decorated_window_count() == 1, "destroyed HWND is pruned");
}

void dpi_changes_keep_text_scale() {
  const SettingsFixture fixture;
  if (!expect(fixture.ready(), "isolate registry and profile store")) return;
  editor::WindowsWindowDecorator decorator;
  constexpr float desktop = 1.15F, stored = 1.30F;
  editor::set_windows_desktop_text_scale(desktop);
  expect(static_cast<bool>(platform::write_editor_text_scale(130)), "seed editor text size");
  editor::load_startup_text_scale();
  Fl_Double_Window window(220, 100, "DPI fixture");
  window.end();
  window.show();
  window.wait_for_expose();
  const auto native = fl_xid(&window);
  const int screen = window.screen_num();
  float dpi{}, vertical{};
  Fl::screen_dpi(dpi, vertical, screen);
  RECT position{};
  if (!expect(native && GetWindowRect(native, &position) && dpi > 0, "shown editor window and initial DPI")) return;
  const auto different = static_cast<WORD>(dpi == 144.0F ? dpi - 24.0F : dpi + 24.0F);
  SendMessageW(native, WM_DPICHANGED, MAKEWPARAM(different, different), reinterpret_cast<LPARAM>(&position));
  const float raw = static_cast<float>(different) / 96.0F;
  expect(std::abs(Fl::screen_scale(screen) - raw) < 1e-3F, "FLTK drops the multiplier on a sent same-screen DPI change");
  InvalidateRect(native, nullptr, FALSE);
  expect(wait_for([&] { return std::abs(Fl::screen_scale(screen) - raw * desktop * stored) < 1e-3F; }),
         "next paint restores both multipliers once for the changed screen");
  if (Fl::screen_count() > 1) {
    const int destination = (screen + 1) % Fl::screen_count();
    int x{}, y{}, width{}, height{};
    Fl::screen_xywh(x, y, width, height, destination);
    const float scale = Fl::screen_scale(destination);
    const LONG left = static_cast<LONG>(std::lround(static_cast<float>(x + width / 2) * scale));
    const LONG top = static_cast<LONG>(std::lround(static_cast<float>(y + height / 2) * scale));
    RECT suggested{left - 100, top - 50, left + 100, top + 50};
    float destination_dpi{};
    Fl::screen_dpi(destination_dpi, vertical, destination);
    const auto message_dpi = static_cast<WORD>(destination_dpi);
    SendMessageW(native, WM_DPICHANGED, MAKEWPARAM(message_dpi, message_dpi), reinterpret_cast<LPARAM>(&suggested));
    InvalidateRect(native, nullptr, FALSE);
    Fl::check();
    expect(window.screen_num() == destination && std::abs(Fl::screen_scale(destination) - scale) < 1e-3F,
           "a cross-screen move retains the destination's already-composed scale");
    SendMessageW(native, WM_DPICHANGED, MAKEWPARAM(different, different), reinterpret_cast<LPARAM>(&position));
  } else {
    std::puts("[skip] Synthetic cross-screen WM_DPICHANGED needs a second logical display.");
  }
  const auto original = static_cast<WORD>(dpi);
  SendMessageW(native, WM_DPICHANGED, MAKEWPARAM(original, original), reinterpret_cast<LPARAM>(&position));
  InvalidateRect(native, nullptr, FALSE);
  Fl::check();
  editor::set_windows_desktop_text_scale(1.0F);
  expect(static_cast<bool>(platform::write_editor_text_scale(100)), "reset test text size");
  editor::load_startup_text_scale();
}

} // namespace

// An owned renderer fixture, not a shipping option. No capture preference is
// written and the refusal path does not change real desktop settings.
static int show_capture_options_evidence(const bool refuse) {
  const SettingsFixture fixture;
  if (!fixture.ready()) return 2;
  Fl::keyboard_screen_scaling(0);
  fl_open_display();
  editor::install_editor_style();
  editor::EditorApp app;
  editor::WindowsWindowDecorator decorator(+[](HWND, bool hide) { return !hide; });
  app.build();
  decorator.set_warning_reporter([&app](std::string_view warning) {
    app.set_capture_exclusion_warning(warning);
  });
  editor::set_editor_capture_exclusion(refuse);
  app.show();
  Fl::add_timeout(0.0, [](void *data) {
    auto &owner = *static_cast<editor::EditorApp *>(data);
    editor::show_options_dialog(owner);
    owner.close_window();
  }, &app);
  return Fl::run();
}

int main(const int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--evidence-capture-refused")
    return show_capture_options_evidence(true);
  if (argc == 2 && std::string_view(argv[1]) == "--evidence-capture-off")
    return show_capture_options_evidence(false);
  if (argc == 2 && (std::string_view(argv[1]) == "--evidence-contrast" ||
                    std::string_view(argv[1]) == "--evidence-tooltip")) {
    if (GetEnvironmentVariableW(L"LAYERC_PROFILE_DIR", nullptr, 0) <= 1 ||
        GetEnvironmentVariableW(L"LAYERC_SETTINGS_KEY", nullptr, 0) <= 1) return 2;
    if (std::string_view(argv[1]) == "--evidence-contrast")
      platform::set_native_high_contrast_override(true);
    else
      Fl::add_timeout(0.25, show_evidence_tooltip);
    return editor::run_windows_editor(GetModuleHandleW(nullptr), L"");
  }
  Fl::keyboard_screen_scaling(0);
  fl_open_display();
  return layerc::test::run({
      LAYERC_TEST_CASE(appearance_mapping),
      LAYERC_TEST_CASE(shared_appearance_record),
      LAYERC_TEST_CASE(startup_migrates_legacy_appearance),
      LAYERC_TEST_CASE(options_store),
      LAYERC_TEST_CASE(options_dialog_actions_and_descriptions),
      LAYERC_TEST_CASE(capture_exclusion_on_existing_and_new_windows),
      LAYERC_TEST_CASE(capture_exclusion_failure_and_recovery),
      LAYERC_TEST_CASE(text_scale_startup_and_keys),
      LAYERC_TEST_CASE(contrast_mapping_and_desktop_listener),
      LAYERC_TEST_CASE(appearance_contrast_theme_sets_floors_aside),
      LAYERC_TEST_CASE(appearance_shared_record_floors),
      LAYERC_TEST_CASE(appearance_primary_button_and_system_floors),
      LAYERC_TEST_CASE(caption_decoration_and_pruning),
      LAYERC_TEST_CASE(dpi_changes_keep_text_scale),
      LAYERC_TEST_CASE(editor_tooltips_keys_and_menu),
      LAYERC_TEST_CASE(stepped_fields_and_preview_nudge),
  });
}
