#include "editor_model.hpp"
#include "profile_edit.hpp"
#include "test_support.hpp"
#include "layerc/platform/profile_bundle_load.hpp"

#include <sys/stat.h>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>

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
} // namespace

LAYERC_TEST_MAIN(
    LAYERC_TEST_CASE(edit_save_roundtrip_and_disconnected),
    LAYERC_TEST_CASE(selection_survives_save_rescan),
    LAYERC_TEST_CASE(invalid_input_is_atomic),
    LAYERC_TEST_CASE(write_failure_and_invalid_module_are_visible))
