#include "automation_presets.hpp"
#include "binding_capture.hpp"
#include "document_commit.hpp"
#include "profile_document.hpp"
#include "profile_edit.hpp"
#include "test_support.hpp"

#include "layerc/platform/profile_bundle_load.hpp"
#include "layerc/platform/profile_store.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

void editor_cooldown_removal_contracts();

namespace {
using namespace layerc::core;
using layerc::editor::CommitResult;
using layerc::editor::ProfileDocument;
using layerc::test::expect;

// Isolated store roots for both platform layouts; never the user's store.
class IsolatedStore {
public:
  IsolatedStore()
      : temp_("layerc-editor-store"),
        profiles_("LAYERC_PROFILE_DIR", temp_.path() / "profiles"),
        home_("HOME", temp_.path()),
        config_("XDG_CONFIG_HOME", temp_.path() / "config"),
        data_("XDG_DATA_HOME", temp_.path() / "data") {
    directory_ = layerc::platform::profile_directory();
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    expect(!error && !directory_.empty(), "an isolated profile store exists");
  }
  [[nodiscard]] const std::filesystem::path &directory() const noexcept {
    return directory_;
  }

private:
  layerc::test::TempDirectory temp_;
  layerc::test::ScopedEnvironment profiles_;
  layerc::test::ScopedEnvironment home_;
  layerc::test::ScopedEnvironment config_;
  layerc::test::ScopedEnvironment data_;
  std::filesystem::path directory_;
};

ProfileDocument full_document() {
  ProfileDocument document;
  document.profile() = layerc::editor::make_new_profile();
  document.profile().game.executable_name = "fixture.exe";
  document.visual().emplace();
  document.visual()->observations.push_back(
      PixelRule{.id = "ready", .name = "Ready"});
  document.visual()->triggers.push_back(
      {.cooldown_id = "ability", .observation_id = "ready"});
  document.mark_visual_edited();
  document.automation().emplace();
  document.automation()->automations.push_back(
      layerc::editor::make_automation_preset(
          layerc::editor::AutomationPreset::input_cooldown, "auto", "ability",
          "main"));
  document.mark_automation_edited();
  document.reset_history();
  return document;
}

std::optional<layerc::platform::ProfileBundleLoad>
load(const std::filesystem::path &path) {
  auto loaded = layerc::platform::load_profile_bundle(path);
  if (auto *bundle = std::get_if<layerc::platform::ProfileBundleLoad>(&loaded))
    return std::move(*bundle);
  return std::nullopt;
}

std::optional<std::filesystem::path> save_new(const IsolatedStore &store,
                                              ProfileDocument &document) {
  const auto path = layerc::editor::default_profile_path(store.directory(),
                                                         document.profile());
  const auto outcome =
      layerc::editor::commit_document(document, path, {}, nullptr);
  if (!expect(outcome.ok() && !outcome.write_error,
              "a valid document commits through the durable store"))
    return std::nullopt;
  document.path() = path;
  document.mark_saved();
  return path;
}

// Every editor frontend saves through the same pipeline; a reopened document
// equals the committed one, including both edited modules.
void round_trip_contracts() {
  const IsolatedStore store;
  auto document = full_document();
  const json::Value::Object opaque{{"future", json::Value(std::string("keep me"))}};
  document.profile().extra_fields = opaque;
  document.profile().game.extra_fields = opaque;
  document.profile().cooldowns.front().extra_fields = opaque;
  document.profile().panels.front().extra_fields = opaque;
  document.profile().panels.front().bar.extra_fields = opaque;
  document.profile().modules = {
      {.kind = ProfileModuleKind::visual, .schema_version = 2, .extra_fields = opaque},
      {.kind = ProfileModuleKind::automation, .schema_version = 1, .extra_fields = opaque}};
  document.reset_history();
  document.profile().name = "Edited";
  (void)document.history().record(document.profile(), document.visual(),
                                  document.automation());
  document.set_dirty(true);
  const auto path = save_new(store, document);
  if (!path) return;
  expect(!document.dirty() && !document.history().is_dirty(
                                  document.profile(), document.visual(),
                                  document.automation()),
         "a saved document is clean and its values are the saved baseline");
  expect(document.visual()->profile_id == document.profile().id &&
             document.automation()->profile_id == document.profile().id,
         "commit aligns sidecar profile ids with the base profile");

  auto loaded = load(*path);
  if (!expect(loaded.has_value(), "the saved bundle loads")) return;
  ProfileDocument reopened;
  reopened.assign_bundle(*path, std::move(*loaded));
  // The store declares the sidecars it wrote; everything else round-trips.
  auto expected = document.profile();
  expected.modules = reopened.profile().modules;
  const auto stored = serialize_profile_json(reopened.profile());
  const auto wanted = serialize_profile_json(expected);
  expect(stored == wanted,
         "reopening yields the exact committed base profile\n" + stored +
             "\n---\n" + wanted);
  expect(reopened.profile().modules.size() == 2,
         "the stored base declares both written modules");
  expect(reopened.profile().modules == document.profile().modules,
         "bundle publication retains opaque module declaration fields");
  reopened.reset_history();
  reopened.profile().name = "Another edit";
  (void)reopened.history().record(reopened.profile(), reopened.visual(), reopened.automation());
  const auto undo = reopened.history().undo(
      reopened.profile(), reopened.visual(), reopened.automation());
  expect(undo && undo->profile.extra_fields == opaque &&
             undo->profile.name == "Edited",
         "document undo retains unknown fields as part of the value snapshot");
  const auto again_saved = layerc::editor::commit_document(reopened, *path, {}, nullptr);
  auto after_edit = load(*path);
  expect(again_saved.ok() && after_edit && after_edit->profile.extra_fields == opaque &&
             after_edit->profile.panels.front().bar.extra_fields == opaque &&
             after_edit->profile.modules == document.profile().modules,
         "a second durable save retains unknown fields throughout the base profile");
  expect(layerc::test::read_file(layerc::platform::recovery_backup_path(*path)) == stored,
         "the recovery copy retains the complete previous profile including opaque data");
  const auto before_rejected = layerc::test::read_file(*path);
  reopened.profile().extra_fields["enabled"] = json::Value(false);
  const auto rejected = layerc::editor::commit_document(reopened, *path, {}, nullptr);
  expect(rejected.result == CommitResult::invalid_profile &&
             layerc::test::read_file(*path) == before_rejected &&
             reopened.profile().extra_fields.contains("enabled"),
         "invalid opaque edits preserve both disk data and the rejected draft");
  reopened.profile().extra_fields.erase("enabled");
  expect(reopened.visual() == document.visual(),
         "reopening yields the exact committed visual module");
  expect(reopened.automation() == document.automation(),
         "reopening yields the exact committed automation module");
  expect(reopened.visual_state() == layerc::platform::ModuleSidecarState::valid &&
             reopened.automation_state() ==
                 layerc::platform::ModuleSidecarState::valid &&
             !reopened.has_unreadable_module(),
         "reopened modules are valid");
  expect(layerc::editor::is_document_path(reopened, *path) &&
             layerc::editor::default_profile_path(store.directory(),
                                                  reopened.profile()) == *path,
         "the same id keeps its store file");

  // Removing a module is an explicit intent that survives a reopen.
  reopened.automation().reset();
  reopened.mark_automation_edited();
  const auto removed =
      layerc::editor::commit_document(reopened, *path, {}, nullptr);
  auto again = load(*path);
  expect(removed.ok() && again &&
             again->modules.get<AutomationModule>().state() ==
                 layerc::platform::ModuleSidecarState::absent,
         "an absent edited module is removed by the commit");
}

// A damaged primary sidecar recovers from its backup; when both are damaged
// the module is unreadable and no commit may discard its recovery evidence.
void recovery_contracts() {
  const IsolatedStore store;
  auto document = full_document();
  const auto path = save_new(store, document);
  if (!path) return;
  const auto paths =
      layerc::platform::sidecar_paths(*path, ProfileModuleKind::automation);
  expect(layerc::test::write_file(paths.primary, "{ damaged"),
         "fixture damages the automation primary");
  auto recovered = load(*path);
  if (!expect(recovered.has_value(), "a damaged sidecar keeps the base loadable"))
    return;
  const auto &automation = recovered->modules.get<AutomationModule>();
  expect(automation.state() ==
                 layerc::platform::ModuleSidecarState::recovered_backup &&
             automation.optional_profile() == document.automation(),
         "the backup recovers the committed automation module");

  expect(layerc::test::write_file(paths.backup, "{ also damaged"),
         "fixture damages the automation backup");
  auto unreadable = load(*path);
  if (!expect(unreadable.has_value(), "the base still loads")) return;
  ProfileDocument blocked;
  blocked.assign_bundle(*path, std::move(*unreadable));
  expect(blocked.has_unreadable_module() && !blocked.automation(),
         "both copies damaged leaves an unreadable module");
  blocked.profile().name = "Unrelated edit";
  const auto outcome =
      layerc::editor::commit_document(blocked, *path, {}, nullptr);
  expect(outcome.result == CommitResult::blocked_by_sidecar,
         "an unrelated edit cannot discard an unreadable module");
  expect(layerc::test::read_file(paths.primary) == std::string("{ damaged") &&
             layerc::test::read_file(paths.backup) ==
                 std::string("{ also damaged"),
         "the blocked commit leaves both recovery copies untouched");
  auto base = load(*path);
  expect(base && base->profile.name == document.profile().name,
         "the blocked commit leaves the stored base untouched");
}

void rejected_commit_contracts() {
  const IsolatedStore store;
  {
    auto document = full_document();
    document.profile().cooldowns.front().duration = std::chrono::milliseconds(0);
    const auto before = document.profile();
    const auto path = store.directory() / "invalid.json";
    const auto outcome =
        layerc::editor::commit_document(document, path, {}, nullptr);
    std::error_code error;
    expect(outcome.result == CommitResult::invalid_profile &&
               !std::filesystem::exists(path, error) &&
               document.profile() == before,
           "an invalid profile writes nothing and keeps the draft");
  }
  {
    auto document = full_document();
    auto bundle = document.bundle();
    bundle.upgrade_needed = true;
    const auto path = store.directory() / "legacy.json";
    int asked = 0;
    const auto declined = layerc::editor::commit_profile_bundle(
        path, bundle, {}, [&] { ++asked; return false; });
    const auto no_prompt = layerc::editor::commit_profile_bundle(
        path, bundle, {.prompt_upgrade = false}, [&] { ++asked; return true; });
    const auto no_callback =
        layerc::editor::commit_profile_bundle(path, bundle, {}, nullptr);
    std::error_code error;
    expect(declined.result == CommitResult::upgrade_declined &&
               no_prompt.result == CommitResult::upgrade_declined &&
               no_callback.result == CommitResult::upgrade_declined &&
               asked == 1 && !std::filesystem::exists(path, error),
           "a one-way upgrade needs explicit consent and writes nothing otherwise");
    const auto accepted = layerc::editor::commit_profile_bundle(
        path, bundle, {}, [&] { ++asked; return true; });
    expect(accepted.ok() && asked == 2 && std::filesystem::exists(path, error),
           "an accepted upgrade commits");
  }
  {
    auto document = full_document();
    const auto outcome =
        layerc::editor::commit_document(document, {}, {}, nullptr);
    expect(outcome.result == CommitResult::write_failed &&
               outcome.write_error ==
                   layerc::platform::ProfileWriteError::invalid_input &&
               layerc::editor::write_failure_text(outcome.write_error)
                       .starts_with("Profile write failed: "),
           "a missing destination is a typed write failure");
  }
}

void profile_edit_contracts() {
  using layerc::editor::parse_duration_seconds;
  expect(parse_duration_seconds("1.5") == Milliseconds(1500) &&
             parse_duration_seconds("0.0004") == std::nullopt &&
             parse_duration_seconds("0") == std::nullopt &&
             parse_duration_seconds("-1") == std::nullopt &&
             parse_duration_seconds("") == std::nullopt &&
             parse_duration_seconds("2s") == std::nullopt &&
             parse_duration_seconds("1e9") == std::nullopt,
         "durations parse as positive bounded seconds");
  {
    auto grouped = layerc::editor::make_new_profile();
    expect(!layerc::editor::split_grouped_bars(grouped, 1000) &&
               grouped == layerc::editor::make_new_profile(),
           "a profile without shared bars is left unchanged");
    grouped.cooldowns.push_back(grouped.cooldowns.front());
    grouped.cooldowns.back().id = "second";
    grouped.panels.front().cooldown_ids.push_back("second");
    grouped.panels.front().bar.height = 18;
    grouped.panels.push_back({.id = "main-ability", .name = "Taken",
                              .style = layerc::core::PanelStyle::card,
                              .cooldown_ids = {"ability"}});
    const auto first = layerc::editor::split_grouped_bars(grouped, 240);
    const auto &panels = grouped.panels;
    expect(first == 0U && panels.size() == 3U && panels[0].id == "main-ability-2" &&
               panels[1].id == "main-second" && panels[2].id == "main-ability" &&
               panels[0].cooldown_ids == std::vector<std::string>{"ability"} &&
               panels[1].cooldown_ids == std::vector<std::string>{"second"} &&
               panels[0].name == "Main ability",
           "a shared bar splits into unique single-cooldown bars in place");
    // (18 + spacing 6) / 240 = 0.1 apart, centred on the shared anchor 0.75.
    expect(std::abs(panels[0].anchor_y - 0.70) < 1e-9 &&
               std::abs(panels[1].anchor_y - 0.80) < 1e-9 &&
               panels[0].anchor_x == 0.5,
           "split bars stack around the shared anchor");
  }
  auto profile = layerc::editor::make_new_profile();
  expect(!profile.game.executable_name.empty() || validate_profile(profile),
         "a new draft must name its application before it can be saved");
  profile.game.executable_name = "game.exe";
  expect(!validate_profile(profile), "a new profile with an application is valid");
  profile.game.executable_name.clear();
  expect(layerc::editor::profile_list_text(profile) == "New Profile",
         "a plain profile lists by name");
  profile.game.executable_name = "game.exe";
  profile.enabled = false;
  expect(layerc::editor::profile_list_text(profile) ==
             "New Profile  \u2014  game.exe  (off)",
         "the list text names the executable and the off state");
  expect(layerc::editor::unique_panel_id("main", profile.panels) == "main-2" &&
             layerc::editor::unique_panel_id("other", profile.panels) == "other",
         "panel ids stay unique");
  expect(layerc::editor::profile_error_line({"$.name", "is empty"}) ==
                 "$.name: is empty" &&
             layerc::editor::profile_error_line({"", "whole"}) == "whole",
         "profile errors render as one line");

  layerc::editor::BindingCapture capture;
  capture.begin_binding_capture();
  capture.record_binding_key(0x11);
  capture.record_binding_key(0x41);
  capture.record_binding_key(0x11);
  expect(capture.capturing_binding() &&
             capture.pending_binding_codes() ==
                 std::vector<std::uint16_t>{0x11, 0x41},
         "binding capture keeps the ordered unique chord");
  capture.finish_binding_capture();
  expect(!capture.capturing_binding() && capture.pending_binding_codes().empty(),
         "finishing capture clears it");
}

void create_only_contracts() {
  const IsolatedStore store;
  auto document = full_document();
  const auto path = save_new(store, document);
  if (!path) return;
  const auto original = layerc::test::read_file(*path);
  document.profile().name = "A colliding draft must not overwrite this";
  const auto rejected = layerc::editor::commit_document(
      document, *path, {.create_only = true}, nullptr);
  expect(rejected.result == CommitResult::write_failed &&
             rejected.write_error == layerc::platform::ProfileWriteError::target_exists &&
             layerc::test::read_file(*path) == original,
         "the shared create-only commit refuses an occupied path under writer ownership");
  const auto new_path = store.directory() / "create-only.json";
  document.profile().id = "create-only";
  expect(layerc::editor::commit_document(
             document, new_path, {.create_only = true}, nullptr).ok(),
         "the shared create-only commit publishes an absent destination");
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(editor_cooldown_removal_contracts),
                 LAYERC_TEST_CASE(round_trip_contracts),
                 LAYERC_TEST_CASE(recovery_contracts),
                 LAYERC_TEST_CASE(rejected_commit_contracts),
                 LAYERC_TEST_CASE(profile_edit_contracts),
                 LAYERC_TEST_CASE(create_only_contracts))
