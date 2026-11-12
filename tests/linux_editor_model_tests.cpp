#include "editor_appearance.hpp"
#include "editor_session.hpp"
#include "profile_edit.hpp"
#include "task_list.hpp"
#include "status_voice.hpp"
#include "test_support.hpp"
#include "layerc/platform/profile_bundle_load.hpp"

#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
using layerc::test::expect;
using layerc::editor::CommitResult;

class Fixture {
public:
  Fixture()
      : temp_("layerc-editor-model"),
        home_("HOME", temp_.path()),
        config_("XDG_CONFIG_HOME", temp_.path() / "config"),
        data_("XDG_DATA_HOME", temp_.path() / "data"),
        runtime_("XDG_RUNTIME_DIR", temp_.path() / "runtime"),
        override_("LAYERC_PROFILE_DIR", std::nullopt) {
    std::filesystem::create_directories(temp_.path() / "runtime");
    expect(::chmod((temp_.path() / "runtime").c_str(), 0700) == 0,
           "the fixture owns its private control directory");
    directory = layerc::platform::profile_directory();
    path = directory / "fixture.json";
    profile = layerc::editor::make_new_profile();
    profile.game.executable_name = "fixture.exe";
    auto other_cooldown = profile.cooldowns.front();
    other_cooldown.id = "unedited-ability";
    other_cooldown.name = "Unedited ability";
    other_cooldown.duration = layerc::core::Milliseconds(12345);
    profile.cooldowns.push_back(other_cooldown);
    auto other_panel = profile.panels.front();
    other_panel.id = "unedited-panel";
    other_panel.name = "Unedited panel";
    other_panel.anchor_x = 0.75;
    other_panel.cooldown_ids = {other_cooldown.id};
    profile.panels.push_back(other_panel);
    expect(layerc::test::write_file(
               path, layerc::core::serialize_profile_json(profile)),
           "write a disposable base profile");
  }
  std::filesystem::path directory;
  std::filesystem::path path;
  layerc::core::GameProfile profile;
private:
  layerc::test::TempDirectory temp_;
  layerc::test::ScopedEnvironment home_, config_, data_, runtime_, override_;
};

void edit_save_roundtrip_and_disconnected() {
  Fixture fixture;
  layerc::editor::EditorSession session(fixture.directory);
  expect(session.profiles().profiles.size() == 1 && !session.open(0),
         "scan and load the XDG profile through the production session");
  expect(!session.edit([](auto &profile) {
    profile.name = "Fixture edited";
    profile.cooldowns.front().name = "Edited ability";
    profile.cooldowns.front().duration = layerc::core::Milliseconds(1250);
    profile.panels.front().anchor_x = 0.2;
  }), "accept one production edit transaction");
  const auto result = session.save([] { return true; });
  expect(result.saved() && !session.document().dirty(), "a durable save marks the document clean");
  expect(result.reload && result.reload->state == layerc::editor::ReloadState::daemon_unavailable,
         "offline saves do not pretend to reload a daemon");
  expect(!result.text.empty(), "the combined disk/runtime outcome is visible");
  auto expected = fixture.profile;
  expected.name = "Fixture edited";
  expected.cooldowns.front().name = "Edited ability";
  expected.cooldowns.front().duration = layerc::core::Milliseconds(1250);
  expected.panels.front().anchor_x = 0.2;
  auto loaded = layerc::platform::load_profile_bundle(fixture.path);
  const auto *bundle = std::get_if<layerc::platform::ProfileBundleLoad>(&loaded);
  expect(bundle && bundle->profile == expected,
         "roundtrip retains all unedited cooldown/panel/binding fields");
}

void selection_survives_save_rescan() {
  Fixture fixture;
  fixture.profile.name = "Alpha";
  expect(layerc::test::write_file(fixture.path,
             layerc::core::serialize_profile_json(fixture.profile)),
         "give the open profile its initial name");
  auto other = layerc::editor::make_new_profile();
  other.id = "other-profile";
  other.name = "Middle";
  other.game.executable_name = "other.exe";
  const auto other_path = fixture.directory / "other.json";
  expect(layerc::test::write_file(other_path,
             layerc::core::serialize_profile_json(other)),
         "create a second selectable profile");
  layerc::editor::EditorSession session(fixture.directory);
  expect(session.profiles().profiles.size() == 2 && !session.open_path(fixture.path),
         "open the intended profile independently of directory order");
  if (session.profiles().profiles.size() != 2) return;
  const auto intended = other_path;
  expect(!session.edit([](auto &profile) { profile.name = "Zulu"; }),
         "rename through the production session");
  expect(session.save([] { return true; }).saved(), "saving a rename rescans the list");
  // Directory iteration order is deliberately unspecified. Path lookup must
  // remain correct after Save rescans, irrespective of its new row position.
  expect(!session.open_path(intended) &&
             session.document().path() == other_path &&
             session.document().profile().id == other.id,
         "a pre-save selection follows its path, not a stale row");
  expect(session.open_path(fixture.directory / "missing.json").has_value() &&
             session.document().path() == other_path,
         "a missing selection leaves the current document intact");
}

void invalid_candidates_are_atomic() {
  Fixture fixture;
  layerc::editor::EditorSession session(fixture.directory);
  expect(!session.open(0), "open the fixture");
  const auto original = layerc::test::read_file(fixture.path);
  const auto reject = [&](const auto &edit) {
    expect(session.edit(edit).has_value(), "invalid production candidate is rejected");
    expect(session.document().profile() == fixture.profile && !session.can_undo() &&
           !session.has_unsaved_changes() && layerc::test::read_file(fixture.path) == original,
           "invalid candidate changes neither accepted values, history nor disk");
  };
  reject([](auto &profile) { profile.name.clear(); });
  reject([](auto &profile) { profile.cooldowns.front().duration = layerc::core::Milliseconds(0); });
  reject([](auto &profile) { profile.panels.front().anchor_x = -0.01; });
  reject([](auto &profile) { profile.panels.front().anchor_x = 1.01; });
}

void write_failure_and_invalid_module_are_visible() {
  Fixture fixture;
  layerc::editor::EditorSession session(fixture.directory);
  expect(!session.open(0), "open the fixture");
  expect(!session.edit([](auto &profile) { profile.name = "Still unsaved"; }),
         "prepare a dirty edit in the production document");
  std::filesystem::remove(fixture.path);
  std::filesystem::create_directory(fixture.path);
  const auto failed = session.save([] { return true; });
  expect(!failed.saved() && failed.commit.result == CommitResult::write_failed &&
             !failed.reload && session.document().dirty() && !failed.text.empty(),
         "a store failure retains the dirty edit and reports failure without reload");
  std::filesystem::remove(fixture.path);
  expect(layerc::test::write_file(fixture.path,
             layerc::core::serialize_profile_json(fixture.profile)),
         "restore the private base profile");
  const auto sidecar = layerc::platform::visual_sidecar_path(fixture.path);
  expect(layerc::test::write_file(sidecar, "{broken"), "write a private invalid sidecar");
  session.rescan();
  expect(!session.open(0), "base profile remains loadable with an invalid module");
  const auto blocked = session.save([] { return true; });
  expect(!blocked.saved() && blocked.commit.result == CommitResult::blocked_by_sidecar &&
             !blocked.reload && !blocked.text.empty(),
         "invalid module data blocks a save visibly");
  expect(layerc::test::read_file(sidecar) == std::optional<std::string>("{broken"),
         "a blocked save preserves invalid module evidence");
}

void native_style_helpers() {
  using namespace layerc::editor;
  // 91/255 of the way toward white on the dark ramp, toward black on light fills.
  expect(control_border(0x1F1F1F) == 0x6E6E6E && control_border(0x2D2D2D) == 0x777777 &&
             control_border(0xFFFFFF) == 0xA4A4A4,
         "flat controls outline 91/255 of the way toward the contrasting extreme");

  expect(parse_desktop_font("Cantarell 11") == DesktopFont{"Cantarell", 15} &&
             parse_desktop_font("Noto Sans, Medium 10") == DesktopFont{"Noto Sans", 13} &&
             parse_desktop_font("Ubuntu Semi-Bold Italic 12") == DesktopFont{"Ubuntu", 16} &&
             parse_desktop_font("DejaVu Sans 13px") == DesktopFont{"DejaVu Sans", 13} &&
             parse_desktop_font("Inter") == DesktopFont{"Inter", std::nullopt} &&
             !parse_desktop_font("   ") && !parse_desktop_font("11"),
         "desktop font descriptions yield the first family and a pixel size");

  // Two settings, least significant byte first: Net/ThemeName then Gtk/FontName.
  const auto setting = [](std::vector<unsigned char> &out, std::string_view name,
                          std::string_view value) {
    out.insert(out.end(), {1, 0, static_cast<unsigned char>(name.size()), 0});
    out.insert(out.end(), name.begin(), name.end());
    out.resize(out.size() + ((4 - name.size() % 4) % 4) + 4); // pad + serial
    out.insert(out.end(), {static_cast<unsigned char>(value.size()), 0, 0, 0});
    out.insert(out.end(), value.begin(), value.end());
    out.resize(out.size() + (4 - value.size() % 4) % 4);
  };
  std::vector<unsigned char> property{0, 0, 0, 0, 7, 0, 0, 0, 2, 0, 0, 0};
  setting(property, "Net/ThemeName", "Adwaita");
  setting(property, "Gtk/FontName", "Cantarell 11");
  expect(xsettings_string(property, "Gtk/FontName") == std::optional<std::string>("Cantarell 11") &&
             !xsettings_string(property, "Gtk/MonospaceFontName"),
         "the XSETTINGS reader finds a string setting after another one");
  auto truncated = property;
  truncated.resize(truncated.size() - 6);
  expect(!xsettings_string(truncated, "Gtk/FontName") &&
             !xsettings_string(std::vector<unsigned char>{2, 0, 0, 0}, "Gtk/FontName"),
         "truncated or foreign XSETTINGS data yields nothing");
}

void appearance_persistence() {
  using namespace layerc::editor;
  const auto root = std::filesystem::temp_directory_path() /
                    ("layerc-appearance-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  const auto path = root / "layerc" / "editor-appearance.json";
  const auto missing = load_editor_appearance(path);
  expect(missing.error.empty() && missing.appearance == EditorAppearance{} &&
             missing.appearance.theme == EditorTheme::copper,
         "a missing appearance file is the Graphite / Copper default without an error");
  EditorAppearance chosen;
  chosen.theme = EditorTheme::teal;
  chosen.accent = 0x123456;
  chosen.overrides[static_cast<std::size_t>(EditorRole::edit)].fill = 0x101010;
  chosen.overrides[static_cast<std::size_t>(EditorRole::status)].text = 0xABCDEF;
  expect(!save_editor_appearance(path, chosen), "an appearance saves durably beside the profiles");
  expect(layerc::test::read_file(path) ==
             std::optional<std::string>("{\"appearanceVersion\":1,\"theme\":\"teal\","
                 "\"accent\":\"#123456\",\"overrides\":{\"edit\":{\"fill\":\"#101010\"},"
                 "\"status\":{\"text\":\"#ABCDEF\"}}}\n"),
         "the appearance file is byte-stable JSON");
  expect(load_editor_appearance(path).appearance == chosen && load_editor_appearance(path).error.empty(),
         "a saved appearance round-trips");
  const auto edit = theme_role_colors(chosen, EditorRole::edit);
  const auto button = theme_role_colors(chosen, EditorRole::button);
  const auto label = theme_role_colors(chosen, EditorRole::label);
  expect(edit && edit->fill == 0x101010 && edit->text == 0xF3F3F3 && button &&
             button->fill == 0x2D2D2D && label && label->text == 0xB8B8B8 &&
             label->fill == 0x1F1F1F,
         "presets use the Windows neutral ramp with per-role overrides");
  EditorAppearance teal;
  teal.theme = EditorTheme::teal;
  // A quarter (60/255) of the teal accent 0x5AB6AA over the 0x1F1F1F window.
  EditorAppearance system;
  system.theme = EditorTheme::system;
  expect(theme_role_colors(teal, EditorRole::list)->selection == 0x2C423F &&
             !theme_role_colors(system, EditorRole::window),
         "selection blends the preset accent; the system theme keeps FLTK colours");
  expect(theme_role_colors(teal, EditorRole::checkbox)->selection == 0x5AB6AA,
         "check marks use the full accent");
  chosen.accent = 0U;
  expect(appearance_json(chosen).find("\"accent\":\"#000000\"") != std::string::npos,
         "explicit black is serialized rather than omitted as the preset");
  expect(!save_editor_appearance(path, chosen), "black accent saves durably");
  expect(load_editor_appearance(path).error.empty() && load_editor_appearance(path).appearance == chosen,
         "explicit black survives a durable round trip");
  expect(theme_role_colors(chosen, EditorRole::list)->selection == 0x181818,
         "explicit black, not the teal preset, is blended into selection");
  chosen.accent.reset();
  const auto preset_json = appearance_json(chosen);
  const auto preset_parsed = parse_appearance(preset_json);
  expect(preset_json.find("\"accent\"") == std::string::npos &&
             std::holds_alternative<EditorAppearance>(preset_parsed) &&
             std::get<EditorAppearance>(preset_parsed) == chosen,
         "an absent accent remains the compatible preset representation");
  const auto legacy = parse_appearance("{\"appearanceVersion\":1,\"theme\":\"teal\"}");
  expect(std::holds_alternative<EditorAppearance>(legacy) &&
             !std::get<EditorAppearance>(legacy).accent &&
             theme_role_colors(std::get<EditorAppearance>(legacy), EditorRole::list)->selection == 0x2C423F,
         "existing version-one files without accent retain the preset");
  const std::string invalid_text = "{\"appearanceVersion\":1,\"theme\":\"neon\"}";
  expect(layerc::test::write_file(path, invalid_text), "write an invalid appearance file");
  const auto invalid = load_editor_appearance(path);
  expect(!invalid.error.empty() && invalid.appearance == EditorAppearance{} &&
             layerc::test::read_file(path) == std::optional<std::string>(invalid_text),
         "an invalid appearance file is reported, ignored and left untouched");
  for (const char *bad : {"[]", "{\"appearanceVersion\":2,\"theme\":\"teal\"}",
                          "{\"appearanceVersion\":1,\"theme\":\"teal\",\"extra\":1}",
                          "{\"appearanceVersion\":1,\"theme\":\"teal\",\"accent\":\"#12345\"}",
                          "{\"appearanceVersion\":1,\"theme\":\"teal\",\"overrides\":{\"menu\":{}}}",
                          "{\"appearanceVersion\":1,\"theme\":\"teal\",\"overrides\":{\"edit\":{\"hover\":\"#000000\"}}}"})
    expect(std::holds_alternative<std::string>(parse_appearance(bad)),
           "malformed appearance JSON is refused");
  std::filesystem::remove_all(root);
}

void detected_tasks_and_placement() {
  using layerc::editor::TaskCandidate;
  TaskCandidate game;
  game.window = 1;
  game.title = "Game";
  game.class_name = "game";
  game.executable_name = "game.bin";
  game.executable_path = "/opt/game/game.bin";
  game.pid = 40U;
  game.width = 1600;
  game.height = 900;
  TaskCandidate xterm;
  xterm.class_name = "xterm";
  TaskCandidate own = game;
  own.window = 2; own.pid = 7U; own.title = "LayerE";
  TaskCandidate hidden = game;
  hidden.window = 3; hidden.hidden = true;
  TaskCandidate untitled = game;
  untitled.window = 4; untitled.title.clear();
  const std::vector<TaskCandidate> all{game, own, hidden, untitled};
  const auto shown = layerc::editor::visible_tasks(all, 7U, false);
  expect(shown.size() == 1U && shown.front() == game,
         "the editor itself, hidden and untitled windows are left out by default");
  expect(layerc::editor::visible_tasks(all, 7U, true).size() == 3U,
         "Show all windows adds hidden and untitled windows but never the editor");
  expect(layerc::editor::task_row_text(untitled) == "game.bin\t(untitled)\t40" &&
             layerc::editor::task_row_text(xterm) == "xterm\t(untitled)\t-",
         "rows show the program, title and PID with explicit placeholders");

  auto profile = layerc::editor::make_new_profile();
  profile.game.window_title = "old";
  auto applied = layerc::editor::apply_task(profile, game, false, true);
  expect(applied.applied && profile.game.executable_name == "game.bin" &&
             profile.game.executable_path.empty() && profile.game.window_title.empty() &&
             profile.name == "game" && profile.id == "game" &&
             applied.status == "Application selected — Save once to create the profile.",
         "an unsaved New Profile takes the executable matcher and its name");
  applied = layerc::editor::apply_task(profile, game, true, false);
  expect(applied.applied && profile.game.executable_path == "/opt/game/game.bin" &&
             profile.name == "game" &&
             applied.status == "Application selected — matching its exact path.",
         "a saved profile keeps its name and can match the exact path");
  TaskCandidate unreadable = game;
  unreadable.executable_name.clear();
  unreadable.executable_path.clear();
  applied = layerc::editor::apply_task(profile, unreadable, true, false);
  expect(applied.applied && profile.game.executable_name.empty() &&
             profile.game.executable_path.empty() && profile.game.window_class == "game",
         "an unreadable process falls back to the window class");
  const auto before = profile;
  applied = layerc::editor::apply_task(profile, TaskCandidate{}, false, false);
  expect(!applied.applied && profile == before,
         "a window without executable or class leaves the profile unchanged");

  const auto frame = layerc::editor::placement_frame(10, 20, 400, 300, 1600, 900);
  expect(frame == layerc::editor::PlacementFrame{10, 57, 400, 225, 0.25},
         "a 16:9 target is aspect-fitted and centred in the preview");
  const auto origin = layerc::editor::placed_origin(frame, 0.5, 0.75, 90, 8);
  expect(origin == layerc::editor::PreviewPoint{165, 222},
         "panels are centred on anchor times the target size, as the X11 overlay does");
  const auto anchor = layerc::editor::anchor_at(frame, 210, 225);
  expect(std::abs(anchor.x - 0.5) < 1e-9 && std::abs(anchor.y - (168.0 / 225.0)) < 1e-9,
         "a preview point maps back to the overlay anchor");
  const auto outside = layerc::editor::anchor_at(frame, -50, 9999);
  expect(outside.x == 0.0 && outside.y == 1.0, "anchors are clamped to the target window");
}

// One token source for every colour, the desktop's text metrics, and the
// status mark rule (DesignSystem.md, "Where each color may go" and Status
// strip).
void editor_token_contracts() {
  using namespace layerc::editor;
  const auto neutral = [](const EditorAppearance &appearance) {
    const auto tokens = editor_tokens(appearance);
    return tokens.background == 0x1F1F1F && tokens.panel == 0x2D2D2D &&
           tokens.input == 0x171717 && tokens.border == 0x6E6E6E &&
           tokens.divider == 0x333333 && tokens.text == 0xF3F3F3 &&
           tokens.secondary == 0xB8B8B8 && tokens.subtext == 0xA3A3A3 &&
           tokens.verbose == 0x9A9A9A && tokens.disabled == 0x808080 &&
           tokens.button_hover == 0x383838 && tokens.row_hover == 0x2A2A2A &&
           tokens.pressed == 0x262626 && tokens.yes == 0x00E676 &&
           tokens.no == 0xFF5A5A && tokens.warning == 0xFFD740 &&
           tokens.on_status == 0x141414;
  };
  const std::array<EditorTheme, 4> presets{EditorTheme::copper, EditorTheme::teal,
                                           EditorTheme::plum, EditorTheme::dark};
  for (const auto theme : presets) {
    EditorAppearance appearance;
    appearance.theme = theme;
    expect(neutral(appearance), "every preset shares the Windows neutral ramp");
    const auto tokens = editor_tokens(appearance);
    // Copper and plum keep the blue mark; teal and slate sit near it and take
    // the violet one, exactly as the Windows theme picks.
    const auto expected = theme == EditorTheme::teal || theme == EditorTheme::dark
                              ? 0xB99BFFU
                              : 0x60B0FFU;
    expect(editor_accent_2(tokens.accent) == expected,
           "the second accent mark is the one far enough from the accent");
    expect(tokens.accent == preset_accent(theme) && tokens.selection != 0,
           "the preset accent and its selection blend come from the same source");
    expect(editor_contrast_ratio(tokens.on_accent, tokens.accent) >= 4.5,
           "ink on the accent clears 4.5:1");
  }
  for (const auto theme : presets) {
  EditorAppearance appearance;
  appearance.theme = theme;
  const auto tokens = editor_tokens(appearance);
  const std::array<std::uint32_t, 4> fills{tokens.background, tokens.input,
                                           tokens.panel, tokens.selection};
  const auto floor = [](std::uint32_t first, std::uint32_t second, double ratio) {
    return editor_contrast_ratio(first, second) >= ratio;
  };
  bool text_floors = true, secondary_floors = true;
  for (const auto fill : fills) {
    text_floors = text_floors && floor(tokens.text, fill, 4.5);
    secondary_floors = secondary_floors && floor(tokens.secondary, fill, 4.5);
    for (const auto mark : {tokens.yes, tokens.warning, tokens.accent_2})
      text_floors = text_floors && floor(mark, fill, 4.5);
    text_floors = text_floors && floor(tokens.accent, fill, 3.0);
  }
  for (const auto fill : {tokens.background, tokens.input, tokens.panel}) {
    secondary_floors = secondary_floors && floor(tokens.subtext, fill, 4.5) &&
                       floor(tokens.verbose, fill, 4.5) &&
                       floor(tokens.no, fill, 4.5) &&
                       floor(tokens.disabled, fill, 3.0) &&
                       floor(control_border(fill), fill, 3.0);
  }
  expect(text_floors, "text, secondary, status marks and the accent clear their floors");
  expect(secondary_floors,
         "hints, verbose text, the failure mark and control outlines clear their floors");
  expect(floor(tokens.no, tokens.selection, 3.0) &&
             floor(tokens.on_accent, tokens.accent, 4.5) &&
             floor(tokens.on_status, tokens.yes, 4.5) &&
             floor(tokens.on_status, tokens.no, 4.5) &&
             floor(tokens.on_status, tokens.warning, 4.5),
         "the marks and the status ink clear their floors");
  }

  expect(desktop_text_metrics(std::nullopt) == DesktopTextMetrics{14, 1.0F} &&
             desktop_text_metrics(10) == DesktopTextMetrics{11, 1.0F} &&
             desktop_text_metrics(13) == DesktopTextMetrics{13, 1.0F} &&
             desktop_text_metrics(14) == DesktopTextMetrics{14, 1.0F} &&
             desktop_text_metrics(21) == DesktopTextMetrics{14, 1.5F},
         "the body size keeps 11..14 px and a larger desktop scales the screens");
}

// Failure, degraded, success and plain information, in that order.
void editor_status_glyph_contracts() {
  using namespace layerc::editor;
  expect(editor_status_glyph("LayerD is not running") == EditorStatusGlyph::no &&
             editor_status_glyph("LayerD is running") == EditorStatusGlyph::yes,
         "a negated state never reads as its success");
  expect(editor_status_glyph("Visual recognition: inactive") ==
                 EditorStatusGlyph::warning &&
             editor_status_glyph("Visual recognition: active") ==
                 EditorStatusGlyph::yes,
         "inactive is degraded, active is a success");
  expect(editor_status_glyph("Saved.") == EditorStatusGlyph::yes &&
             editor_status_glyph("Disconnected.") == EditorStatusGlyph::no,
         "sentence case still classifies, and a refusal stays a failure");
  expect(editor_status_glyph("Not saved. the profile is invalid") ==
             EditorStatusGlyph::no &&
             editor_status_glyph("Profile saved.") == EditorStatusGlyph::yes &&
             editor_status_glyph("Profile loaded.") == EditorStatusGlyph::yes &&
             editor_status_glyph("Could not save the selected options.") ==
                 EditorStatusGlyph::no &&
             editor_status_glyph("") == EditorStatusGlyph::none &&
             editor_status_glyph("Edit the profile fields.") ==
                 EditorStatusGlyph::information,
         "failures come before successes and empty text has no mark");
}

// The same speaking rule the Windows editor applies to wide text
// (DesignSystem.md, Screen readers).
void accessible_status_voice_contracts() {
  using layerc::editor::StatusVoice;
  using layerc::editor::status_is_announced;
  expect(!status_is_announced("", "", StatusVoice::announced) &&
             !status_is_announced("Saved", "", StatusVoice::announced),
         "an empty status line is never spoken");
  expect(!status_is_announced("", "Saved", StatusVoice::silent) &&
             !status_is_announced("Failed", "Failed", StatusVoice::silent),
         "a silent message is never spoken");
  expect(status_is_announced("", "Saved", StatusVoice::announced) &&
             status_is_announced("Saved", "Saved", StatusVoice::announced),
         "an explicit outcome is spoken again even when it repeats");
  expect(status_is_announced("", "Failed", StatusVoice::changed) &&
             !status_is_announced("Failed", "Failed", StatusVoice::changed) &&
             status_is_announced("Failed", "Other", StatusVoice::changed),
         "a repeating failure is spoken once, then only after another one");
}
} // namespace

LAYERC_TEST_MAIN(
    LAYERC_TEST_CASE(edit_save_roundtrip_and_disconnected),
    LAYERC_TEST_CASE(selection_survives_save_rescan),
    LAYERC_TEST_CASE(invalid_candidates_are_atomic),
    LAYERC_TEST_CASE(write_failure_and_invalid_module_are_visible),
    LAYERC_TEST_CASE(appearance_persistence),
    LAYERC_TEST_CASE(native_style_helpers),
    LAYERC_TEST_CASE(accessible_status_voice_contracts),
    LAYERC_TEST_CASE(editor_token_contracts),
    LAYERC_TEST_CASE(editor_status_glyph_contracts),
    LAYERC_TEST_CASE(detected_tasks_and_placement))
