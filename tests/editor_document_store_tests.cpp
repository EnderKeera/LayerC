#include "automation_presets.hpp"
#include "binding_capture.hpp"
#include "document_commit.hpp"
#include "editor_document_store_fixture.hpp"
#include "profile_document.hpp"
#include "profile_edit.hpp"
#include "test_support.hpp"

#include "layerc/platform/profile_bundle_load.hpp"
#include "layerc/platform/profile_store.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

void editor_cooldown_removal_contracts();
void editor_document_ownership_contracts();
void diagnostic_text_contracts();
void profile_scan_summary_preserves_store();
void wire_size_resource_contracts();

namespace {
using namespace layerc::core;
using layerc::editor::CommitResult;
using layerc::editor::DocumentChangeKind;
using layerc::editor::ProfileDocument;
using layerc::test::document_store::fixture_load;
using layerc::test::document_store::full_document;
using layerc::test::document_store::IsolatedStore;
using layerc::test::document_store::load;
using layerc::test::document_store::save_new;
using layerc::test::expect;

// Every editor frontend saves through the same pipeline; a reopened document
// equals the committed one, including both edited modules.
void round_trip_contracts() {
  const IsolatedStore store;
  const json::Value::Object opaque{{"future", json::Value(std::string("keep me"))}};
  auto seeded = layerc::editor::make_new_profile();
  seeded.game.executable_name = "fixture.exe";
  seeded.extra_fields = opaque;
  seeded.game.extra_fields = opaque;
  seeded.cooldowns.front().extra_fields = opaque;
  seeded.panels.front().extra_fields = opaque;
  seeded.panels.front().bar.extra_fields = opaque;
  seeded.modules = {
      {.kind = ProfileModuleKind::visual, .schema_version = 2, .extra_fields = opaque},
      {.kind = ProfileModuleKind::automation, .schema_version = 1, .extra_fields = opaque}};
  VisualProfile seeded_visual;
  seeded_visual.profile_id = seeded.id;
  seeded_visual.observations.push_back(PixelRule{.id = "ready", .name = "Ready"});
  seeded_visual.triggers.push_back({.cooldown_id = "ability", .observation_id = "ready"});
  AutomationProfile seeded_automation;
  seeded_automation.profile_id = seeded.id;
  seeded_automation.automations.push_back(layerc::editor::make_automation_preset(
      layerc::editor::AutomationPreset::input_cooldown, "auto", "ability", "main"));
  ProfileDocument document;
  document.start_draft(std::move(seeded), layerc::editor::DraftOrigin::fresh,
                       std::move(seeded_visual), std::move(seeded_automation));
  expect(document.edit_profile([](auto &candidate) { candidate.name = "Edited"; },
                               DocumentChangeKind::immediate).accepted() &&
             document.dirty(),
         "the opaque fixture rename is one real recorded edit");
  const auto path = save_new(store, document);
  if (!path) return;
  expect(!document.dirty() &&
             document.history().is_saved(document.profile(), document.visual(),
                                         document.automation()),
         "a saved document is clean and its values are the saved baseline");
  expect(document.visual()->profile_id == document.profile().id &&
             document.automation()->profile_id == document.profile().id,
         "commit aligns sidecar profile ids with the base profile");

  auto loaded = load(*path);
  if (!expect(loaded.has_value(), "the saved bundle loads")) return;
  ProfileDocument reopened;
  reopened.open(*path, std::move(*loaded));
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
  expect(reopened.edit_profile([](auto &candidate) { candidate.name = "Another edit"; },
                               DocumentChangeKind::immediate).accepted() &&
             reopened.dirty() && reopened.can_undo(),
         "the second rename is one recorded edit");
  expect(reopened.undo() && reopened.profile().extra_fields == opaque &&
             reopened.profile().name == "Edited",
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
  // A malformed opaque copy draft exercises the refusal without touching the
  // valid reopened document.
  auto malformed_profile = reopened.profile();
  malformed_profile.extra_fields["enabled"] = json::Value(false);
  ProfileDocument malformed;
  malformed.start_draft(malformed_profile, layerc::editor::DraftOrigin::copy,
                        reopened.visual(), reopened.automation());
  const auto rejected = layerc::editor::commit_document(malformed, *path, {}, nullptr);
  expect(rejected.result == CommitResult::invalid_profile &&
             layerc::test::read_file(*path) == before_rejected &&
             malformed.profile().extra_fields.contains("enabled"),
         "invalid opaque edits preserve both disk data and the rejected draft");
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
  expect(reopened.edit_automation([](auto &automation) -> std::optional<std::string> {
           automation.reset();
           return std::nullopt;
         }, DocumentChangeKind::immediate).accepted(),
         "removing the automation module is one recorded edit");
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
  blocked.open(*path, std::move(*unreadable));
  expect(blocked.has_unreadable_module() && !blocked.automation(),
         "both copies damaged leaves an unreadable module");
  expect(blocked.edit_profile([](auto &candidate) { candidate.name = "Unrelated edit"; },
                              DocumentChangeKind::immediate).accepted(),
         "an unrelated base rename is still an accepted recorded edit");
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
    auto seeded = full_document();
    auto malformed = seeded.profile();
    malformed.cooldowns.front().duration = std::chrono::milliseconds(0);
    ProfileDocument document;
    document.start_draft(std::move(malformed), layerc::editor::DraftOrigin::copy,
                         seeded.visual(), seeded.automation());
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

// Every refusal or failure keeps the accepted values in place: a refused save
// must not replace document storage, normalize its modules, or consume the
// legacy-schema consent it never obtained.
// Commits a save the fixture expects to be refused or to fail, and checks the
// document kept its accepted values, identity, consent, history and storage.
layerc::editor::CommitOutcome
attempt_refused(ProfileDocument &document, const std::filesystem::path &path,
                const layerc::editor::CommitOptions &options,
                const layerc::editor::ConfirmUpgrade &confirm,
                const CommitResult expected) {
  const auto before = document.bundle();
  const auto old_path = document.path();
  const bool old_dirty = document.dirty();
  const bool old_base_legacy = document.legacy_base_schema();
  const bool old_visual_legacy = document.legacy_visual_schema();
  const bool old_ack = document.upgrade_acknowledged();
  const bool old_undo = document.history().can_undo();
  const bool old_redo = document.history().can_redo();
  const auto *cooldowns = document.profile().cooldowns.data();
  const auto *panels = document.profile().panels.data();
  const auto *observations =
      document.visual() ? document.visual()->observations.data() : nullptr;
  const auto *automations =
      document.automation() ? document.automation()->automations.data() : nullptr;
  const auto outcome =
      layerc::editor::commit_document(document, path, options, confirm);
  expect(outcome.result == expected,
         "the intended save refusal/failure is exercised");
  expect(document.profile() == before.profile &&
             document.visual() == before.visual &&
             document.automation() == before.automation &&
             document.path() == old_path && document.dirty() == old_dirty &&
             document.legacy_base_schema() == old_base_legacy &&
             document.legacy_visual_schema() == old_visual_legacy &&
             document.upgrade_acknowledged() == old_ack &&
             document.history().can_undo() == old_undo &&
             document.history().can_redo() == old_redo,
         "a refused save preserves accepted values, identity, schema consent and history");
  expect(document.profile().cooldowns.data() == cooldowns &&
             document.profile().panels.data() == panels &&
             (!document.visual() ||
              document.visual()->observations.data() == observations) &&
             (!document.automation() ||
              document.automation()->automations.data() == automations),
         "a refused save cannot invalidate references into unchanged document storage");
  return outcome;
}

// Each refusal fixture is a separate function so /analyze bounds the stack of
// every case independently (C6262).
void refused_invalid_candidate(const IsolatedStore &store) {
  auto seeded = full_document();
  auto malformed = seeded.profile();
  malformed.cooldowns.front().duration = std::chrono::milliseconds(0);
  ProfileDocument document;
  document.start_draft(std::move(malformed), layerc::editor::DraftOrigin::copy,
                       seeded.visual(), seeded.automation());
  const auto path = store.directory() / "invalid-storage.json";
  const auto outcome =
      attempt_refused(document, path, {}, nullptr, CommitResult::invalid_profile);
  std::error_code error;
  expect(outcome.result == CommitResult::invalid_profile &&
             !std::filesystem::exists(path, error),
         "an invalid candidate writes nothing");
}

void refused_legacy_upgrade(const IsolatedStore &store) {
  auto seeded = full_document();
  auto loaded = fixture_load(seeded.profile(), seeded.visual(),
                             seeded.automation());
  loaded.legacy_base_schema = true;
  ProfileDocument document;
  document.open({}, std::move(loaded));
  const auto path = store.directory() / "legacy-storage.json";
  (void)attempt_refused(document, path, {}, [] { return false; },
                        CommitResult::upgrade_declined);
  (void)attempt_refused(document, path, {.prompt_upgrade = false},
                        [] { return true; }, CommitResult::upgrade_declined);
  (void)attempt_refused(document, path, {}, nullptr,
                        CommitResult::upgrade_declined);
  std::error_code error;
  expect(document.legacy_base_schema() && !document.upgrade_acknowledged() &&
             !std::filesystem::exists(path, error),
         "a refused upgrade keeps the legacy schema unacknowledged and writes nothing");
}

void refused_blocked_sidecar(const IsolatedStore &store) {
  auto seeded = full_document();
  auto loaded = fixture_load(seeded.profile(), seeded.visual());
  loaded.modules.get<AutomationModule>() =
      layerc::platform::AutomationSidecarLoad::invalid(
          {.path = "$", .message = "fixture invalid sidecar"}, {});
  ProfileDocument document;
  document.open({}, std::move(loaded));
  const auto path = store.directory() / "blocked-storage.json";
  (void)attempt_refused(document, path, {}, nullptr,
                        CommitResult::blocked_by_sidecar);
  std::error_code error;
  expect(document.automation_state() ==
                 layerc::platform::ModuleSidecarState::invalid &&
             !document.automation() && !std::filesystem::exists(path, error),
         "a blocked commit preserves the unreadable module and writes nothing");
}

void refused_write_failures(const IsolatedStore &store) {
  {
    auto document = full_document();
    const auto outcome =
        attempt_refused(document, {}, {}, nullptr, CommitResult::write_failed);
    expect(outcome.write_error ==
               layerc::platform::ProfileWriteError::invalid_input,
           "a missing destination is a typed write failure without document mutation");
  }
  auto document = full_document();
  const auto path = store.directory() / "directory-storage.json";
  std::error_code error;
  std::filesystem::create_directory(path, error);
  expect(!error, "the fixture creates a directory at the destination");
  (void)attempt_refused(document, path, {}, nullptr, CommitResult::write_failed);
  expect(std::filesystem::is_directory(path, error),
         "a failed durable write leaves the existing destination in place");
}

void refused_commit_preserves_storage_contracts() {
  const IsolatedStore store;
  refused_invalid_candidate(store);
  refused_legacy_upgrade(store);
  refused_blocked_sidecar(store);
  refused_write_failures(store);
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

// Snapshots are whole bundle copies; building and comparing them in this
// helper's frame keeps each case's stack bounded under /analyze (C6262).
bool document_equals(const ProfileDocument &document,
                     const layerc::editor::EditorProfileSnapshot &expected) {
  return layerc::editor::EditorProfileSnapshot{
             document.profile(), document.visual(), document.automation()} ==
         expected;
}

int undo_depth(const ProfileDocument &document) {
  auto probe = document;
  int depth{};
  while (probe.undo()) ++depth;
  return depth;
}

void enablement_policy_contracts() {
  using layerc::editor::EnablementRefusal;
  using layerc::editor::OpenDocumentEnablement;
  using layerc::editor::ProfileBundle;
  using layerc::editor::profile_enablement_bundle;
  const IsolatedStore store;
  auto document = full_document();
  const auto path = save_new(store, document);
  if (!path) return;
  auto stored = load(*path);
  if (!expect(stored.has_value(), "the enablement fixture bundle loads")) return;
  const auto stored_bundle = layerc::editor::loaded_bundle(std::move(*stored));
  expect(document.edit_profile([](auto &candidate) { candidate.name = "Unsaved open edits"; },
                               DocumentChangeKind::immediate).accepted() &&
             document.dirty(),
         "the open enablement fixture carries one recorded edit");
  auto current = profile_enablement_bundle(
      document, true, *path, OpenDocumentEnablement::commit_open_edits);
  const auto *open_bundle = std::get_if<ProfileBundle>(&current);
  expect(open_bundle && open_bundle->profile == document.profile() &&
             open_bundle->visual == document.visual() &&
             open_bundle->automation == document.automation() && document.dirty(),
         "Windows enablement explicitly includes the open unsaved bundle");
  const auto refused = profile_enablement_bundle(
      document, true, *path, OpenDocumentEnablement::require_clean);
  expect(std::get_if<EnablementRefusal>(&refused) &&
             std::get<EnablementRefusal>(refused) == EnablementRefusal::dirty_open_document,
         "Linux enablement explicitly refuses dirty open documents");
  // A second document opened from the store is clean by construction (derived
  // dirty cannot be cleared by hand). Rewrite only the stored file: require_clean
  // must return the new disk values while the open document keeps its own.
  ProfileDocument reopened;
  auto reloaded = load(*path);
  if (!expect(reloaded.has_value(), "the enablement fixture reloads")) return;
  reopened.open(*path, std::move(*reloaded));
  const auto in_memory = reopened.profile();
  auto stored_replacement = in_memory;
  stored_replacement.name = "Stored replacement";
  stored_replacement.enabled = false;
  expect(!reopened.dirty() &&
             layerc::platform::write_profile_file(
                 *path, serialize_profile_json(stored_replacement)).ok(),
         "a clean reopened document is not dirty while the stored file changes");
  const auto clean = profile_enablement_bundle(
      reopened, true, *path, OpenDocumentEnablement::require_clean);
  const auto *clean_bundle = std::get_if<ProfileBundle>(&clean);
  expect(clean_bundle && clean_bundle->profile == stored_replacement &&
             reopened.profile() == in_memory && !reopened.dirty(),
         "clean-only enablement reads the stored bundle, not in-memory values");
  expect(reopened.edit_profile([](auto &candidate) {
           candidate.name = "Unsaved again";
         }, layerc::editor::DocumentChangeKind::immediate).accepted(),
         "an accepted in-memory edit is recorded");
  const auto after_edit = profile_enablement_bundle(
      reopened, true, *path, OpenDocumentEnablement::require_clean);
  expect(std::get_if<EnablementRefusal>(&after_edit) &&
             std::get<EnablementRefusal>(after_edit) ==
                 EnablementRefusal::dirty_open_document,
         "an accepted in-memory edit makes clean-only enablement refuse");
  expect(document.edit_profile([](auto &candidate) {
           candidate.name = "Still unsaved";
         }, layerc::editor::DocumentChangeKind::immediate).accepted(),
         "the open document keeps a recorded in-memory edit");
  const auto open_before_noncurrent = layerc::editor::EditorProfileSnapshot{
      document.profile(), document.visual(), document.automation()};
  const auto path_before_noncurrent = document.path();
  const auto dirty_before_noncurrent = document.dirty();
  const auto undo_before_noncurrent = document.can_undo();
  const auto redo_before_noncurrent = document.can_redo();
  for (const auto policy : {OpenDocumentEnablement::commit_open_edits,
                            OpenDocumentEnablement::require_clean}) {
    const auto other = profile_enablement_bundle(document, false, *path, policy);
    const auto *bundle = std::get_if<ProfileBundle>(&other);
    expect(bundle && bundle->profile == stored_replacement &&
               bundle->visual == stored_bundle.visual &&
               bundle->automation == stored_bundle.automation,
           "both policies load non-current rows from disk despite open edits");
  }
  expect(document_equals(document, open_before_noncurrent) &&
             document.path() == path_before_noncurrent &&
             document.dirty() == dirty_before_noncurrent &&
             document.can_undo() == undo_before_noncurrent &&
             document.can_redo() == redo_before_noncurrent,
         "noncurrent enablement bundle selection never changes the open document or history");
  const auto missing = profile_enablement_bundle(
      document, false, store.directory() / "missing.json",
      OpenDocumentEnablement::commit_open_edits);
  expect(std::get_if<EnablementRefusal>(&missing) &&
             std::get<EnablementRefusal>(missing) == EnablementRefusal::unreadable,
         "an unreadable stored profile has a typed refusal");
  const auto invalid_path = store.directory() / "invalid.json";
  expect(layerc::test::write_file(invalid_path, "{ invalid"),
         "invalid enablement fixture should be written");
  const auto document_before_invalid = document.profile();
  const auto invalid_undo_depth = undo_depth(document);
  const auto invalid = profile_enablement_bundle(
      document, false, invalid_path, OpenDocumentEnablement::require_clean);
  expect(std::get_if<EnablementRefusal>(&invalid) &&
             std::get<EnablementRefusal>(invalid) == EnablementRefusal::invalid_profile &&
             document.profile() == document_before_invalid &&
             undo_depth(document) == invalid_undo_depth,
         "invalid stored profiles are distinct from unreadable ones and leave the document alone");
  const auto enabled_before = layerc::editor::EditorProfileSnapshot{
      reopened.profile(), reopened.visual(), reopened.automation()};
  const auto depth_before = undo_depth(reopened);
  const auto committed_enablement = layerc::editor::commit_document(
      reopened, *path, {}, [] { return true; },
      layerc::platform::KeepModule{}, {},
      [](ProfileBundle &bundle) { bundle.profile.enabled = !bundle.profile.enabled; });
  if (!expect(committed_enablement.ok() && !reopened.dirty() &&
                  reopened.profile().enabled != enabled_before.profile.enabled &&
                  undo_depth(reopened) == depth_before + 1,
              "current-row candidate enablement durably accepts exactly one new history step"))
    return;
  const auto enabled_after = layerc::editor::EditorProfileSnapshot{
      reopened.profile(), reopened.visual(), reopened.automation()};
  expect(reopened.undo() && document_equals(reopened, enabled_before) &&
             reopened.dirty() && reopened.path() == *path,
         "one undo restores the complete pre-enablement bundle without changing the path");
  expect(reopened.redo() && document_equals(reopened, enabled_after) &&
             !reopened.dirty() && !reopened.can_redo(),
         "one redo restores the exact saved current-row enablement bundle");
}

void history_nested_unknown_fields_contracts() {
  const IsolatedStore store;
  auto seeded_document = full_document();
  const auto unknown_result =
      json::parse(R"json([{"text":"retained"},9007199254740993])json");
  const auto *unknown_value = std::get_if<json::Value>(&unknown_result);
  if (!expect(unknown_value != nullptr && unknown_value->is_array() &&
                  unknown_value->as_array().size() == 2,
              "nested history value with an exact integer parses")) return;
  const auto &unknown_array = unknown_value->as_array();
  const auto *unknown_text = unknown_array[0].find("text");
  if (!expect(unknown_text != nullptr && unknown_text->is_string() &&
                  unknown_text->as_string() == "retained" &&
                  unknown_array[1].is_integer() &&
                  unknown_array[1].as_integer() ==
                      std::int64_t{9'007'199'254'740'993},
              "history fixture contains its nested string and exact integer")) return;
  const json::Value::Object unknown_fields{{"futureHistory", *unknown_value}};
  auto seeded_profile = seeded_document.profile();
  seeded_profile.extra_fields = unknown_fields;
  ProfileDocument document;
  document.open(store.directory() / "structured.json",
                fixture_load(std::move(seeded_profile),
                             seeded_document.visual(),
                             seeded_document.automation()));
  const auto saved_profile = document.profile();
  expect(document.edit_profile([](auto &candidate) {
           candidate.name = "Structured rename";
         }, DocumentChangeKind::immediate).accepted() &&
             document.dirty() && document.can_undo(),
         "structured rename is one recorded edit");
  const auto renamed_profile = document.profile();
  expect(document.history().is_baseline(document.profile(), document.visual(),
                                        document.automation()) &&
             !document.history().is_saved(document.profile(), document.visual(),
                                          document.automation()) &&
             document.history().is_dirty(document.profile(), document.visual(),
                                         document.automation()),
         "recorded rename is the dirty baseline, not the saved value");
  expect(document.undo() && document.profile() == saved_profile &&
             document.profile().extra_fields == unknown_fields,
         "undo retains nested unknown array, string and exact integer fields");
  expect(document.history().is_saved(document.profile(), document.visual(),
                                     document.automation()) &&
             !document.history().is_dirty(document.profile(), document.visual(),
                                          document.automation()),
         "undo returns to the saved baseline");
  expect(document.redo() && document.profile() == renamed_profile &&
             document.profile().extra_fields == unknown_fields,
         "redo retains nested unknown array, string and exact integer fields");
  expect(document.history().is_baseline(document.profile(), document.visual(),
                                        document.automation()) &&
             !document.history().is_saved(document.profile(), document.visual(),
                                          document.automation()) &&
             document.history().is_dirty(document.profile(), document.visual(),
                                         document.automation()),
         "redo restores the dirty structured baseline");

  const auto saved_path = save_new(store, document);
  if (!saved_path) return;
  expect(!document.dirty() &&
             document.history().is_saved(document.profile(), document.visual(),
                                         document.automation()) &&
             document.history().is_baseline(document.profile(), document.visual(),
                                            document.automation()),
         "mark_saved makes the committed profile the clean saved baseline");

  auto saved_history = document.history();
  const auto committed_profile = document.profile();
  auto after_save_edit = committed_profile;
  after_save_edit.name = "Structured rename after save";
  if (!expect(saved_history.record(after_save_edit, document.visual(),
                                  document.automation()),
              "a post-save structured rename records history")) return;
  expect(saved_history.is_baseline(after_save_edit, document.visual(),
                                  document.automation()) &&
             !saved_history.is_saved(after_save_edit, document.visual(),
                                    document.automation()) &&
             saved_history.is_dirty(after_save_edit, document.visual(),
                                    document.automation()),
         "post-save rename is dirty relative to mark_saved");
  const auto post_save_undo = saved_history.undo(
      after_save_edit, document.visual(), document.automation());
  if (!expect(post_save_undo.has_value(), "post-save rename can be undone")) return;
  expect(post_save_undo->profile == committed_profile &&
             post_save_undo->profile.extra_fields == unknown_fields &&
             saved_history.is_saved(post_save_undo->profile, document.visual(),
                                    document.automation()),
         "post-save undo restores the saved value and nested unknown fields");
  const auto post_save_redo = saved_history.redo(
      committed_profile, document.visual(), document.automation());
  expect(post_save_redo.has_value() &&
             post_save_redo->profile == after_save_edit &&
             post_save_redo->profile.extra_fields == unknown_fields &&
             saved_history.is_dirty(post_save_redo->profile, document.visual(),
                                    document.automation()),
         "post-save redo retains nested unknown fields and dirty semantics");
}

// A successful durable commit aligns both sidecar ids and adopts clean values.
void module_identity_commit_contracts() {
  const IsolatedStore store;
  auto profile = layerc::editor::make_new_profile();
  profile.game.executable_name = "fixture.exe";
  VisualProfile visual;
  visual.observations.push_back(PixelRule{.id = "ready", .name = "Ready"});
  visual.triggers.push_back({.cooldown_id = "ability", .observation_id = "ready"});
  AutomationProfile automation;
  automation.automations.push_back(layerc::editor::make_automation_preset(
      layerc::editor::AutomationPreset::input_cooldown, "auto", "ability",
      "main"));
  const auto profile_id = profile.id;
  ProfileDocument document;
  document.start_draft(std::move(profile), layerc::editor::DraftOrigin::fresh,
                       std::move(visual), std::move(automation));
  expect(document.visual()->profile_id.empty() &&
             document.automation()->profile_id.empty(),
         "normalization fixture begins with unaligned sidecar ids");
  const auto path = save_new(store, document);
  if (!path) return;
  expect(document.visual()->profile_id == profile_id &&
             document.automation()->profile_id == profile_id &&
             !document.dirty(),
         "a successful durable commit aligns both sidecar ids and adopts its clean values");
}

void overlay_item_store_schema_contracts() {
  const IsolatedStore store;
  const auto path = store.directory() / "schema-compatibility.json";
  const auto check = [&](const GameProfile &profile) {
    const auto expected = serialize_profile_json(profile);
    const auto saved = layerc::platform::write_profile_bundle(
        path, profile, layerc::platform::ProfileModuleIntents{});
    if (!expect(saved.ok(), "schema compatibility fixture durably saves"))
      return;
    expect(layerc::test::read_file(path) == expected,
           "durable bundle save uses the compatibility serializer byte-for-byte");
    const auto loaded = layerc::platform::load_profile_bundle(path);
    const auto *bundle = std::get_if<layerc::platform::ProfileBundleLoad>(&loaded);
    expect(bundle && !bundle->legacy_base_schema &&
               bundle->profile.schema_version == profile_serialization_schema(profile),
           "supported schema4 and schema5 loads do not request an automatic upgrade");
  };
  auto legacy = layerc::editor::make_new_profile();
  legacy.game.executable_name = "fixture.exe";
  legacy.panels.front().extra_fields.emplace(
      "items", json::Value(std::string("opaque schema4 data")));
  legacy.cooldowns.front().extra_fields.emplace("activeDurationMs", json::Value(true));
  check(legacy);

  auto typed = layerc::editor::make_new_profile();
  typed.game.executable_name = "fixture.exe";
  typed.schema_version = profile_schema_version;
  typed.panels.front().items = std::vector<OverlayItem>{
      OverlayItem{.id = "stacks", .content = CounterItem{.variable = "stacks"}}};
  typed.panels.front().cooldown_ids.clear();
  check(typed);

  auto duration = layerc::editor::make_new_profile();
  duration.game.executable_name = "fixture.exe";
  duration.cooldowns.front().active_duration = Milliseconds{1000};
  check(duration);
}

void create_only_contracts() {
  const IsolatedStore store;
  auto document = full_document();
  const auto path = save_new(store, document);
  if (!path) return;
  const auto original = layerc::test::read_file(*path);
  expect(document.edit_profile([](auto &candidate) {
           candidate.name = "A colliding draft must not overwrite this";
         }, DocumentChangeKind::immediate).accepted(),
         "the colliding draft rename is one recorded edit");
  const auto rejected = layerc::editor::commit_document(
      document, *path, {.create_only = true}, nullptr);
  expect(rejected.result == CommitResult::write_failed &&
             rejected.write_error == layerc::platform::ProfileWriteError::target_exists &&
             layerc::test::read_file(*path) == original,
         "the shared create-only commit refuses an occupied path under writer ownership");
  const auto new_path = store.directory() / "create-only.json";
  // A different identity is a NEW copy draft: the base and both module values
  // move together so the candidate is internally consistent.
  auto copy_profile = document.profile();
  copy_profile.id = "create-only";
  copy_profile.name = "Create only";
  auto copy_visual = document.visual();
  auto copy_automation = document.automation();
  if (copy_visual) copy_visual->profile_id = copy_profile.id;
  if (copy_automation) copy_automation->profile_id = copy_profile.id;
  ProfileDocument copy;
  copy.start_draft(std::move(copy_profile), layerc::editor::DraftOrigin::copy,
                   std::move(copy_visual), std::move(copy_automation));
  expect(layerc::editor::commit_document(
             copy, new_path, {.create_only = true}, nullptr).ok(),
         "the shared create-only commit publishes an absent destination");
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(editor_cooldown_removal_contracts),
                 LAYERC_TEST_CASE(round_trip_contracts),
                 LAYERC_TEST_CASE(profile_scan_summary_preserves_store),
                 LAYERC_TEST_CASE(history_nested_unknown_fields_contracts),
                 LAYERC_TEST_CASE(wire_size_resource_contracts),
                 LAYERC_TEST_CASE(recovery_contracts),
                 LAYERC_TEST_CASE(rejected_commit_contracts),
                 LAYERC_TEST_CASE(refused_commit_preserves_storage_contracts),
                 LAYERC_TEST_CASE(profile_edit_contracts),
                 LAYERC_TEST_CASE(create_only_contracts),
                 LAYERC_TEST_CASE(module_identity_commit_contracts),
                 LAYERC_TEST_CASE(enablement_policy_contracts),
                 LAYERC_TEST_CASE(overlay_item_store_schema_contracts),
                 LAYERC_TEST_CASE(editor_document_ownership_contracts),
                 LAYERC_TEST_CASE(diagnostic_text_contracts))
