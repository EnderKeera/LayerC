#include "editor_appearance.hpp"
#include "editor_model.hpp"
#include "profile_edit.hpp"
#include "task_list.hpp"
#include "test_support.hpp"
#include "layerc/platform/profile_bundle_load.hpp"

#include <sys/stat.h>
#include <unistd.h>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
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
  layerc::editor::LinuxEditorModel model(fixture.directory);
  expect(model.profiles().profiles.size() == 1 && !model.open(0),
         "scan and load the XDG profile through shared readers");
  auto fields = model.fields();
  fields.name = "Fixture edited";
  fields.cooldown_name = "Edited ability";
  fields.duration_seconds = "1.25";
  fields.panel_x = "0.2";
  const auto result = model.save(fields, [] { return true; });
  expect(result.saved() && !model.document().dirty(),
         "a durable save marks the document clean");
  expect(result.reload &&
             result.reload->state == layerc::editor::ReloadState::daemon_unavailable,
         "offline saves do not pretend to reload a daemon");
  expect(!result.text.empty(), "the combined disk/runtime outcome is visible");
  auto expected = fixture.profile;
  expected.name = fields.name;
  expected.cooldowns.front().name = fields.cooldown_name;
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
  layerc::editor::LinuxEditorModel model(fixture.directory);
  expect(model.profiles().profiles.size() == 2 && !model.open_path(fixture.path),
         "open the intended profile independently of directory order");
  if (model.profiles().profiles.size() != 2) return;
  const auto intended = other_path;
  auto fields = model.fields();
  fields.name = "Zulu";
  expect(model.save(fields, [] { return true; }).saved(),
         "saving a rename rescans the list");
  // Directory iteration order is deliberately unspecified. Path lookup must
  // remain correct after Save rescans, irrespective of its new row position.
  expect(!model.open_path(intended) &&
             model.document().path() == other_path &&
             model.document().profile().id == other.id,
         "a pre-save selection follows its path, not a stale row");
  expect(model.open_path(fixture.directory / "missing.json").has_value() &&
             model.document().path() == other_path,
         "a missing selection leaves the current document intact");
}

void invalid_input_is_atomic() {
  Fixture fixture;
  layerc::editor::LinuxEditorModel model(fixture.directory);
  expect(!model.open(0), "open the fixture");
  const auto original = layerc::test::read_file(fixture.path);
  for (const auto &value : {"", "nan", "-1", "1garbage", "999999999999"}) {
    auto fields = model.fields();
    fields.duration_seconds = value;
    fields.name = "Must not leak";
    const auto result = model.save(fields, [] { return true; });
    expect(!result.saved() && result.commit.result == CommitResult::invalid_fields &&
               !result.reload && result.text.starts_with("Not saved:"),
           "invalid duration reports a field error without requesting reload");
    expect(model.document().profile() == fixture.profile &&
               layerc::test::read_file(fixture.path) == original,
           "an invalid candidate changes neither document nor disk");
  }
  for (const auto &value : {"nan", "1.01", "-0.01", "0.5junk"}) {
    auto fields = model.fields();
    fields.panel_x = value;
    expect(model.apply(fields).has_value(), "invalid panel anchor is rejected");
  }
}

void write_failure_and_invalid_module_are_visible() {
  Fixture fixture;
  layerc::editor::LinuxEditorModel model(fixture.directory);
  expect(!model.open(0), "open the fixture");
  auto fields = model.fields();
  fields.name = "Still unsaved";
  std::filesystem::remove(fixture.path);
  std::filesystem::create_directory(fixture.path);
  const auto failed = model.save(fields, [] { return true; });
  expect(!failed.saved() && failed.commit.result == CommitResult::write_failed &&
             !failed.reload && model.document().dirty() && !failed.text.empty(),
         "a store failure retains the dirty edit and reports failure without reload");
  std::filesystem::remove(fixture.path);
  expect(layerc::test::write_file(fixture.path,
             layerc::core::serialize_profile_json(fixture.profile)),
         "restore the private base profile");
  const auto sidecar = layerc::platform::visual_sidecar_path(fixture.path);
  expect(layerc::test::write_file(sidecar, "{broken"), "write a private invalid sidecar");
  model.scan();
  expect(!model.open(0), "base profile remains loadable with an invalid module");
  const auto blocked = model.save(model.fields(), [] { return true; });
  expect(!blocked.saved() && blocked.commit.result == CommitResult::blocked_by_sidecar &&
             !blocked.reload && !blocked.text.empty(),
         "invalid module data blocks a save visibly");
  expect(layerc::test::read_file(sidecar) == std::optional<std::string>("{broken"),
         "a blocked save preserves invalid module evidence");
}

void appearance_persistence() {
  using namespace layerc::editor;
  const auto root = std::filesystem::temp_directory_path() /
                    ("layerc-appearance-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  const auto path = root / "layerc" / "editor-appearance.json";
  const auto missing = load_editor_appearance(path);
  expect(missing.error.empty() && missing.appearance == EditorAppearance{},
         "a missing appearance file is the system default without an error");
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
  expect(theme_role_colors(teal, EditorRole::list)->selection == 0x2C423F &&
             !theme_role_colors(EditorAppearance{}, EditorRole::window),
         "selection blends the preset accent; the system theme keeps FLTK colours");
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
} // namespace

LAYERC_TEST_MAIN(
    LAYERC_TEST_CASE(edit_save_roundtrip_and_disconnected),
    LAYERC_TEST_CASE(selection_survives_save_rescan),
    LAYERC_TEST_CASE(invalid_input_is_atomic),
    LAYERC_TEST_CASE(write_failure_and_invalid_module_are_visible),
    LAYERC_TEST_CASE(appearance_persistence),
    LAYERC_TEST_CASE(detected_tasks_and_placement))
