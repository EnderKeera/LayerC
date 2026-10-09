#include "editor_session.hpp"
#include "profile_mutations.hpp"
#include "profile_edit.hpp"
#include "automation_presets.hpp"
#include "test_support.hpp"
#include "layerc/core/companion.hpp"
#include "layerc/platform/profile_bundle_load.hpp"

#include <sys/stat.h>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace {
namespace core = layerc::core;
namespace platform = layerc::platform;
namespace editor = layerc::editor;
namespace bundle = layerc::bundle;
using layerc::test::expect;

class Fixture {
public:
  Fixture()
      : temp_("layerc-editor-session"), home_("HOME", temp_.path()),
        config_("XDG_CONFIG_HOME", temp_.path() / "config"),
        data_("XDG_DATA_HOME", temp_.path() / "data"),
        runtime_("XDG_RUNTIME_DIR", temp_.path() / "runtime"),
        override_("LAYERC_PROFILE_DIR", std::nullopt) {
    std::filesystem::create_directories(temp_.path() / "runtime");
    expect(::chmod((temp_.path() / "runtime").c_str(), 0700) == 0,
           "the control namespace is private and disposable");
    directory = platform::profile_directory();
    path = directory / "fixture.json";
    external = temp_.path() / "export";
    std::filesystem::create_directories(external);
    profile = editor::make_new_profile();
    profile.game.executable_name = "fixture.exe";
  }
  void store(const bool with_modules = false) {
    if (!with_modules) {
      expect(platform::write_profile_file(path, core::serialize_profile_json(profile)).ok(),
             "store the private base profile");
      return;
    }
    core::VisualProfile visual;
    visual.profile_id = profile.id;
    core::PixelRule pixel;
    pixel.id = "ready";
    pixel.name = "Ready";
    visual.observations.push_back(pixel);
    core::AutomationProfile automation;
    automation.profile_id = profile.id;
    automation.automations.push_back(editor::make_automation_preset(
        editor::AutomationPreset::input_cooldown, "auto", "ability", "main"));
    core::CompanionProfile companion;
    companion.profile_id = profile.id;
    core::CompanionSlotMapping slot;
    slot.panel_id = "main";
    companion.slots.push_back(slot);
    expect(platform::write_profile_bundle(
               path, profile, platform::ProfileModuleIntents{visual, automation, companion}).ok(),
           "store all three modules under the existing durable writer");
  }
  std::filesystem::path directory, path, external;
  core::GameProfile profile;
private:
  layerc::test::TempDirectory temp_;
  layerc::test::ScopedEnvironment home_, config_, data_, runtime_, override_;
};

std::optional<platform::ProfileBundleLoad> load(const std::filesystem::path &path) {
  auto result = platform::load_profile_bundle(path);
  if (auto *loaded = std::get_if<platform::ProfileBundleLoad>(&result))
    return std::move(*loaded);
  return std::nullopt;
}

void drafts_and_history() {
  Fixture fixture;
  editor::EditorSession session(fixture.directory);
  expect(!session.has_document() && session.profiles().profiles.empty(),
         "an empty store has no accidental selection");
  session.new_profile();
  expect(session.is_draft() && session.has_unsaved_changes(),
         "a new draft is unsaved even before the first edit");
  expect(!session.edit([](auto &profile) { profile.game.executable_name = "draft.exe"; }),
         "give the draft a valid matching executable");
  expect(!session.edit([](auto &profile) { profile.name = "Draft"; }, true) &&
             !session.edit([](auto &profile) { profile.name = "Draft typed"; }, true),
         "successive text edits coalesce");
  expect(session.undo() && session.document().profile().name == "New Profile" &&
             session.redo() && session.document().profile().name == "Draft typed",
         "one undo and redo restore a coalesced text edit");
  session.end_text_edit();
  expect(!session.edit([](auto &profile) { profile.name = "Saved name"; }, true),
         "a new text field edit gets a new history entry");
  const auto saved = session.save([] { return true; });
  expect(saved.saved() && saved.reload &&
             saved.reload->state == editor::ReloadState::daemon_unavailable &&
             !session.has_unsaved_changes() && !session.is_draft(),
         "a durable save is clean and reports the private daemon absent");
  expect(session.undo() && session.has_unsaved_changes() && session.redo() &&
             !session.has_unsaved_changes(),
         "history crosses the saved baseline without losing cleanliness");
  const auto first_id = session.document().profile().id;
  session.new_profile();
  expect(session.document().profile().id != first_id && session.is_draft(),
         "a second New gets an unused profile identity");
  expect(!session.edit([](auto &profile) { profile.game.executable_name = "next.exe"; }),
         "the next draft is valid");
  expect(session.save([] { return true; }).saved() &&
             session.profiles().profiles.size() == 2,
         "saving a new draft never replaces the previous New profile");
}

void mutation_candidates_and_preservation() {
  Fixture fixture;
  fixture.profile.bindings.push_back(
      {"ability", core::InputChord{{{core::InputDevice::controller, 1, 0}}}});
  fixture.store(true);
  editor::EditorSession session(fixture.directory);
  if (!expect(!session.open(0), "open the complete private bundle")) return;
  const auto before = session.document().profile();
  expect(session.edit([](auto &profile) { profile.panels.clear(); }).has_value() &&
             session.document().profile() == before,
         "a candidate cannot invalidate a preserved automation or companion reference");
  expect(session.edit([](auto &profile) { profile.name.clear(); }).has_value() &&
             session.document().profile() == before && !session.can_undo(),
         "an invalid edit is atomic and adds no history entry");
  std::string added;
  expect(!session.edit([&](auto &profile) { added = editor::add_cooldown(profile); }),
         "add a cooldown with a fresh id");
  std::string panel;
  expect(!session.edit([&](auto &profile) { panel = editor::add_panel(profile); }),
         "add another panel");
  expect(!session.edit([&](auto &profile) {
           (void)editor::set_panel_member(profile, panel, added, false);
         }), "panel membership is editable");
  expect(session.remove_cooldown("ability").has_value() &&
             session.document().profile().cooldowns.front().id == "ability",
         "an automation dependency refuses a destructive cooldown edit");
  expect(!session.remove_cooldown(added), "an unreferenced cooldown can be removed");
  expect(session.save([] { return true; }).saved(), "save accepted mutations");
  const auto reopened = load(fixture.path);
  expect(reopened && reopened->profile.bindings == before.bindings &&
             reopened->modules.get<core::VisualModule>().usable() &&
             reopened->modules.get<core::AutomationModule>().usable() &&
             reopened->modules.get<core::CompanionModule>().usable(),
         "unedited controller bindings and all read-only modules survive Save");
  const auto original_id = session.document().profile().id;
  if (!expect(!session.duplicate_current() && session.is_draft(),
              "a complete profile can be duplicated")) return;
  const auto copy_id = session.document().profile().id;
  expect(!session.edit([](auto &profile) { profile.name = "Edited copy"; }),
         "a duplicate with preserved modules remains editable before its first Save");
  expect(copy_id != original_id && session.save([] { return true; }).saved(),
         "the duplicate has a fresh identity and saves independently");
  const auto copied = load(session.document().path());
  expect(copied && copied->profile.bindings == before.bindings &&
             copied->modules.get<core::VisualModule>().usable() &&
             copied->modules.get<core::AutomationModule>().usable() &&
             copied->modules.get<core::CompanionModule>().usable() &&
             copied->modules.get<core::VisualModule>().profile()->profile_id == copy_id &&
             copied->modules.get<core::AutomationModule>().profile()->profile_id == copy_id &&
             copied->modules.get<core::CompanionModule>().profile()->profile_id == copy_id,
         "a duplicate keeps visual, automation and companion data aligned");
  expect(platform::read_profile_file(fixture.path).has_value(),
         "duplication never modifies or deletes the source");
}

void save_and_delete_failures() {
  Fixture fixture;
  fixture.store(true);
  editor::EditorSession session(fixture.directory);
  if (!expect(!session.open(0), "open the profile to exercise failures")) return;
  const auto original = layerc::test::read_file(fixture.path);
  session.new_profile();
  expect(!session.edit([&](auto &profile) {
           profile = fixture.profile;
           profile.name = "Cannot overwrite";
         }), "prepare a draft with a colliding identity");
  const auto collision = session.save([] { return true; });
  expect(!collision.saved() &&
             collision.commit.write_error == platform::ProfileWriteError::target_exists &&
             !collision.reload && session.is_draft() && session.has_unsaved_changes() &&
             layerc::test::read_file(fixture.path) == original,
         "create-only is checked under transaction ownership and retains the draft");
  expect(!session.open_path(fixture.path), "reopen the saved source");
  const auto automation_path = platform::automation_sidecar_path(fixture.path);
  const auto bad_backup = platform::recovery_backup_path(automation_path);
  std::filesystem::remove(bad_backup);
  std::filesystem::create_directory(bad_backup);
  expect(!session.delete_current().ok && session.has_document() &&
             layerc::test::read_file(fixture.path) == original,
         "a deletion planning failure leaves the base and document intact");
  std::filesystem::remove(bad_backup);
  expect(session.delete_current().ok && !session.has_document() &&
             !std::filesystem::exists(fixture.path),
         "confirmed deletion removes the saved base");
  for (const auto &desc : core::ProfileModuleTable::descriptors) {
    const auto paths = platform::sidecar_paths(fixture.path, desc.kind);
    expect(!std::filesystem::exists(paths.primary) &&
               !std::filesystem::exists(paths.backup) &&
               !std::filesystem::exists(paths.marker),
           "deletion removes every module file, backup and marker atomically");
  }
}

void enablement_and_invalid_modules() {
  Fixture fixture;
  fixture.store(true);
  editor::EditorSession session(fixture.directory);
  if (!expect(!session.open(0), "open for enablement")) return;
  expect(session.set_enabled(0, false, [] { return true; }).ok &&
             !session.document().profile().enabled && !session.has_unsaved_changes(),
         "disablement publishes only the saved profile and refreshes cleanliness");
  expect(!session.edit([](auto &profile) { profile.name = "Pending"; }),
         "prepare pending edits");
  expect(!session.set_enabled(0, true, [] { return true; }).ok &&
             !load(fixture.path)->profile.enabled && session.document().dirty(),
         "enablement cannot silently publish or discard pending edits");
  expect(session.undo(), "undo back to the saved baseline");
  expect(session.set_enabled(0, true, [] { return true; }).ok,
         "enablement succeeds from the saved baseline");
  const auto bad = platform::visual_sidecar_path(fixture.path);
  std::filesystem::remove(platform::recovery_backup_path(bad));
  expect(layerc::test::write_file(bad, "{broken"), "inject invalid module evidence without a usable backup");
  expect(!session.open_path(fixture.path), "an invalid module does not hide the base");
  expect(session.duplicate_current().has_value() && !session.is_draft(),
         "duplication refuses to drop an unreadable module");
  const auto blocked = session.save([] { return true; });
  expect(!blocked.saved() && blocked.commit.result == editor::CommitResult::blocked_by_sidecar &&
             layerc::test::read_file(bad) == std::optional<std::string>("{broken"),
         "saving never repairs invalid module evidence by discarding it");
}

void reviewed_import_export() {
  Fixture fixture;
  fixture.store(true);
  editor::EditorSession session(fixture.directory);
  if (!expect(!session.open(0), "select a profile for export")) return;
  const auto pack = fixture.external / "fixture.layercprofile";
  const auto json = fixture.external / "fixture.json";
  expect(session.export_pack(pack, {0}).ok && session.export_json(json, 0).ok,
         "saved profiles export through the pack writer and durable JSON primitive");
  const auto original = layerc::test::read_file(fixture.path);
  expect(!session.export_pack(fixture.path, {0}).ok &&
             !session.export_json(fixture.path, 0).ok &&
             layerc::test::read_file(fixture.path) == original,
         "export cannot bypass the store transaction by overwriting a live profile");
  auto result = session.plan_import(pack);
  auto *plan = std::get_if<editor::ImportPlan>(&result);
  if (!expect(plan && plan->rows.size() == 1 && plan->rows.front().collides,
              "a pack preview shows the colliding profile")) return;
  plan->rows.front().action = bundle::ImportAction::skip;
  expect(!session.import_pack(*plan).ok,
         "changing an import action requires a new complete preview");
  result = session.review_import(std::move(*plan));
  plan = std::get_if<editor::ImportPlan>(&result);
  if (!expect(plan && session.import_pack(*plan).ok &&
                  session.profiles().profiles.size() == 1 &&
                  layerc::test::read_file(fixture.path) == original,
              "a reviewed skip publishes nothing")) return;
  result = session.plan_import(pack);
  plan = std::get_if<editor::ImportPlan>(&result);
  if (!expect(plan && session.import_pack(*plan).ok &&
                  session.profiles().profiles.size() == 2,
              "a reviewed copy keeps the existing profile")) return;
  result = session.plan_import(json);
  plan = std::get_if<editor::ImportPlan>(&result);
  if (!expect(plan != nullptr, "plain validated JSON uses the same reviewed importer")) return;
  plan->rows.front().action = bundle::ImportAction::replace;
  result = session.review_import(std::move(*plan));
  plan = std::get_if<editor::ImportPlan>(&result);
  if (!expect(plan != nullptr, "review a JSON replacement of the original")) return;
  expect(!session.edit([](auto &profile) { profile.name = "Pending"; }),
         "prepare an edit before import");
  expect(!session.import_pack(*plan).ok && session.document().dirty(),
         "import cannot overwrite the open document's unsaved edits");
  expect(session.undo() && session.import_pack(*plan).ok,
         "a reviewed replacement succeeds after the edit is undone");
  const auto replaced = load(fixture.path);
  expect(replaced && replaced->modules.get<core::VisualModule>().usable() &&
             replaced->modules.get<core::AutomationModule>().usable() &&
             replaced->modules.get<core::CompanionModule>().usable() &&
             !session.has_unsaved_changes(),
         "base JSON replacement keeps all module data and refreshes the open document");
}

void import_destination_race_and_invalid_json() {
  Fixture fixture;
  fixture.store();
  editor::EditorSession session(fixture.directory);
  const auto json = fixture.external / "fixture.json";
  expect(session.export_json(json, 0).ok, "export the input JSON");
  auto result = session.plan_import(json);
  auto *plan = std::get_if<editor::ImportPlan>(&result);
  if (!expect(plan != nullptr, "preview a copy")) return;
  auto raced = fixture.profile;
  raced.id = "another-writer";
  expect(platform::write_profile_file(*plan->reviewed.front().expected_target,
             core::serialize_profile_json(raced)).ok(),
         "another writer occupies the reviewed destination");
  expect(!session.import_pack(*plan).ok,
         "a moved copy destination is rejected, never silently rebound at commit");
  session.rescan();
  expect(session.profiles().profiles.size() == 2,
         "the failed import publishes no partial copy");
  const auto broken = fixture.external / "broken.json";
  expect(layerc::test::write_file(broken, "{broken"), "write an invalid import");
  expect(std::holds_alternative<std::string>(session.plan_import(broken)) &&
             session.profiles().profiles.size() == 2,
         "invalid JSON import is rejected without changing the store");
}

void field_parser_contracts() {
  expect(editor::parse_anchor("0.25") == 0.25 && !editor::parse_anchor("nan") &&
             !editor::parse_anchor("1.01") && !editor::parse_anchor("0.2junk"),
         "anchor parsing requires a complete finite normalized value");
  expect(editor::parse_rgb("#aB12ef") == 0xAB12EFU &&
             !editor::parse_rgb("#fff") && !editor::parse_rgb("#zzzzzz"),
         "RGB values are exact six-digit hexadecimal colors");
  expect(editor::parse_count("8", 1, 8) == 8U && !editor::parse_count("9", 1, 8) &&
             !editor::parse_count("1x", 1, 8),
         "integer fields honor the parser-derived limits");
  const auto profile = editor::make_new_profile();
  expect(editor::chord_label(profile.bindings.front().chord, profile) == "E" &&
             editor::format_seconds(core::Milliseconds(1234)) == "1.234" &&
             editor::format_rgb(0xAB12EFU) == "#AB12EF",
         "display formatting round-trips values and names the physical binding");
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(drafts_and_history),
                 LAYERC_TEST_CASE(mutation_candidates_and_preservation),
                 LAYERC_TEST_CASE(save_and_delete_failures),
                 LAYERC_TEST_CASE(enablement_and_invalid_modules),
                 LAYERC_TEST_CASE(reviewed_import_export),
                 LAYERC_TEST_CASE(import_destination_race_and_invalid_json),
                 LAYERC_TEST_CASE(field_parser_contracts))
