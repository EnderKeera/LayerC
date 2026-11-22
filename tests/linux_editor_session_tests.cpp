#include "editor_session.hpp"
#include "profile_mutations.hpp"
#include "profile_edit.hpp"
#include "automation_presets.hpp"
#include "automation_graph.hpp"
#include "automation_simulator.hpp"
#include "layerc/platform/text.hpp"
#include "visual_calibration.hpp"
#include "layerc/platform/profile_file_guard.hpp"
#include "editor_test_environment.hpp"
#include "template_png_fixture.hpp"
#include <array>
#include <span>
#include "layerc/core/companion.hpp"
#include "layerc/platform/profile_bundle_load.hpp"
#ifdef _WIN32
#include "layerc/platform/win32_handle.hpp"
#endif

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
      : temp_("layerc-editor-session", layerc::test::EditorTestScope::store,
              "the control namespace is private and disposable") {
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
  layerc::test::EditorTestEnvironment temp_;
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
  const std::string utf8 = "\xE6\x97\xA5\xE6\x9C\xAC-\xC3\xA9";
  const auto directory = platform::path_from_utf8(utf8);
  const auto pack_name = platform::path_from_utf8(utf8 + ".layercprofile");
  const auto trace_name = platform::path_from_utf8(utf8 + ".layerctrace");
  if (!expect(directory && pack_name && trace_name, "decode the UTF-8 fixture paths")) return;
  const auto external = fixture.external / *directory;
  std::filesystem::create_directories(external);
  const auto pack = external / *pack_name;
  const auto json = external / "fixture.json";
  const auto packed = session.export_pack(pack, {0});
  const auto exported = session.export_json(json, 0);
  expect(packed.ok && packed.text.find(utf8 + ".layercprofile") != std::string::npos &&
             exported.ok && exported.text.find("fixture.json") != std::string::npos,
         "pack and JSON export through a UTF-8 directory and report the filenames");
  const auto trace = external / *trace_name;
  expect(editor::write_trace_file(trace, editor::trace_from_events({})) &&
             std::holds_alternative<editor::LoadedTrace>(editor::read_trace_file(trace)),
         "automation trace save and load use the converted path");
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

void automation_candidates_history_and_recovery() {
  Fixture fixture;
  fixture.store(true);
  editor::EditorSession session(fixture.directory);
  expect(!session.open(0), "open a complete bundle for automation edits");
  const auto base = session.document().profile();
  const auto initial = session.document().automation();
  const auto disk = layerc::test::read_file(platform::automation_sidecar_path(fixture.path));
  expect(!session.edit_automation([](auto &graph) -> std::optional<std::string> {
           graph->automations.front().name = "Edited automation";
           return std::nullopt;
         }) && session.has_unsaved_changes() && session.can_undo(),
         "accepted automation edits enter shared document history");
  const auto edited = session.document().automation();
  expect(layerc::test::read_file(platform::automation_sidecar_path(fixture.path)) == disk,
         "automation editing never publishes before explicit Save");
  expect(session.edit_automation([](auto &graph) -> std::optional<std::string> {
           graph->profile_id = "wrong";
           return std::nullopt;
         }).has_value() && session.document().automation() == edited,
         "validate the complete module identity before accepting a candidate");
  expect(session.edit_automation([](auto &graph) -> std::optional<std::string> {
           graph.reset();
           return "refused";
         }).has_value() && session.document().automation() == edited,
         "a callback refusal cannot partially remove the module");
  expect(!session.edit_automation([](auto &) -> std::optional<std::string> {
           return std::nullopt;
         }), "a no-op module edit is accepted without new history");
  expect(session.undo() && session.document().automation() == initial &&
             !session.has_unsaved_changes() && session.redo() &&
             session.document().automation() == edited,
         "module undo and redo cross the saved baseline correctly");
  expect(session.document().profile() == base &&
             session.save([] { return true; }).saved(),
         "editing automation does not alter the base and saves durably");
  const auto reopened = load(fixture.path);
  expect(reopened && reopened->modules.get<core::AutomationModule>().usable() &&
             *reopened->modules.get<core::AutomationModule>().profile() == *edited &&
             reopened->modules.get<core::VisualModule>().usable() &&
             reopened->modules.get<core::CompanionModule>().usable(),
         "automation round-trip keeps unrelated modules intact");
  expect(!session.edit_automation([](auto &graph) -> std::optional<std::string> {
           graph.reset();
           return std::nullopt;
         }) && session.document().automation_state() == platform::ModuleSidecarState::absent,
         "an explicit removal updates the sidecar intent");
  expect(session.undo() && session.document().automation_state() == platform::ModuleSidecarState::valid &&
             session.redo() && session.document().automation_state() == platform::ModuleSidecarState::absent,
         "history restores the module state as well as its value");
  expect(session.save([] { return true; }).saved(), "save the removal intent");
  const auto removed = load(fixture.path);
  expect(removed && removed->modules.get<core::AutomationModule>().state() ==
                         platform::ModuleSidecarState::absent,
         "the durable tombstone deactivates automation while retaining recovery bytes");
  fixture.store(true);
  const auto bad = platform::automation_sidecar_path(fixture.path);
  std::filesystem::remove(platform::recovery_backup_path(bad));
  expect(layerc::test::write_file(bad, "{broken") && !session.open_path(fixture.path),
         "load unreadable automation with no recovery copy");
  bool called{};
  expect(session.edit_automation([&](auto &graph) -> std::optional<std::string> {
           called = true;
           graph.reset();
           return std::nullopt;
         }).has_value() && !called && session.document().has_unreadable_module(),
         "an unreadable module cannot become an implicit deletion intent");
  const auto blocked = session.save([] { return true; });
  expect(!blocked.saved() && !blocked.reload &&
             layerc::test::read_file(bad) == std::optional<std::string>("{broken"),
         "failed module edits and saves preserve the original recovery evidence");
}

void visual_candidates_history_and_recovery() {
  Fixture fixture;
  fixture.store(true);
  editor::EditorSession session(fixture.directory);
  expect(!session.open(0), "open a complete bundle for visual edits");
  const auto base = session.document().profile();
  const auto automation = session.document().automation();
  const auto initial = session.document().visual();
  const auto disk = layerc::test::read_file(platform::visual_sidecar_path(fixture.path));
  expect(!session.edit_visual([](auto &visual) -> std::optional<std::string> {
           std::get<core::PixelRule>(visual->observations.front()).name = "Edited observation";
           return std::nullopt;
         }) && session.has_unsaved_changes() && session.can_undo(),
         "visual edits are one validated history transaction");
  const auto edited = session.document().visual();
  expect(layerc::test::read_file(platform::visual_sidecar_path(fixture.path)) == disk,
         "visual editing never publishes until an explicit Save");
  expect(session.edit_visual([](auto &visual) -> std::optional<std::string> {
           visual->profile_id = "wrong";
           return std::nullopt;
         }).has_value() && session.document().visual() == edited,
         "invalid candidates leave the whole document unchanged");
  expect(session.edit_visual([](auto &visual) -> std::optional<std::string> {
           visual.reset();
           return "refused";
         }).has_value() && session.document().visual() == edited,
         "a refused callback cannot partially remove the visual module");
  expect(session.undo() && session.document().visual() == initial &&
             !session.has_unsaved_changes() && session.redo() &&
             session.document().visual() == edited,
         "visual undo/redo cross the saved baseline without altering the base");
  expect(session.document().profile() == base && session.document().automation() == automation &&
             session.save([] { return true; }).saved(), "save the accepted visual edit");
  const auto reopened = load(fixture.path);
  expect(reopened && reopened->modules.get<core::VisualModule>().optional_profile() == edited &&
             reopened->modules.get<core::AutomationModule>().optional_profile() == automation &&
             reopened->modules.get<core::CompanionModule>().usable(),
         "visual round-trip preserves the unrelated modules");
  expect(!session.edit_visual([](auto &visual) -> std::optional<std::string> {
           visual.reset();
           return std::nullopt;
         }) && session.document().visual_state() == platform::ModuleSidecarState::absent &&
             session.undo() && session.document().visual_state() == platform::ModuleSidecarState::valid &&
             session.redo() && session.document().visual_state() == platform::ModuleSidecarState::absent,
         "module removals and history keep the visual sidecar state synchronized");
  expect(session.save([] { return true; }).saved(), "save the explicit removal intent");
  const auto removed = load(fixture.path);
  expect(removed && removed->modules.get<core::VisualModule>().state() ==
                        platform::ModuleSidecarState::absent,
         "visual removal is a durable tombstone, not loss of recovery files");

  fixture.store(true);
  const auto bad = platform::visual_sidecar_path(fixture.path);
  std::filesystem::remove(platform::recovery_backup_path(bad));
  expect(layerc::test::write_file(bad, "{broken") && !session.open_path(fixture.path),
         "open unreadable visual data without a recovery copy");
  const auto diagnostic = session.document().visual_error();
  bool called{};
  expect(session.edit_visual([&](auto &visual) -> std::optional<std::string> {
           called = true;
           visual.reset();
           return std::nullopt;
         }).has_value() && !called && session.document().has_unreadable_module(),
         "ordinary editing never converts unreadable data to removal");
  const auto invalid_calibration =
      session.prepare_calibration("ability", core::VisualProfile{}, {}, {});
  expect(std::get_if<std::string>(&invalid_calibration) != nullptr &&
             session.document().has_unreadable_module(),
         "invalid calibration cannot weaken the unreadable-sidecar gate");
  const auto model = editor::CalibrationModel::open(base, "ability", std::nullopt);
  expect(model.has_value(), "seed an explicit repair calibration");
  if (!model) return;
  auto result = model->build_result(model->initial_controls(), {});
  const auto *visual = std::get_if<core::VisualProfile>(&result);
  expect(visual != nullptr, "a pixel calibration builds one valid observation and trigger");
  if (!visual) return;
  auto prepared_repair = session.prepare_calibration("ability", *visual, {}, {});
  auto *accepted_repair = std::get_if<editor::AcceptedCalibration>(&prepared_repair);
  expect(accepted_repair != nullptr, "a valid repair calibration prepares");
  expect(accepted_repair &&
             !session.apply_calibration(std::move(*accepted_repair)) &&
             session.document().visual_state() == platform::ModuleSidecarState::valid &&
             session.document().dirty() && session.undo() &&
             session.document().has_unreadable_module() &&
             session.document().visual_error() == diagnostic &&
             !session.has_unsaved_changes(),
         "undoing an unsaved calibration repair restores the unreadable state and diagnostic");
  const auto blocked = session.save([] { return true; });
  expect(!blocked.saved() && !blocked.reload &&
             layerc::test::read_file(bad) == std::optional<std::string>("{broken"),
         "Save after repair undo preserves all original failure evidence");
  expect(session.redo() && !session.document().has_unreadable_module(),
         "redo restores the explicit validated repair");
  {
#ifdef _WIN32
    platform::OwnedHandle lock(CreateFileW(bad.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!expect(lock.valid(), "prevent replacement of the disposable visual sidecar")) return;
#else
    const auto scope = platform::profile_transaction_scope(fixture.directory);
    if (!expect(scope.has_value(), "resolve the private transaction scope")) return;
    platform::TransactionFileLock lock(*scope);
    expect(lock.locked(), "take the disposable store writer lock");
#endif
    const auto failed = session.save([] { return true; });
    expect(!failed.saved() && !failed.reload && session.document().dirty() &&
               layerc::test::read_file(bad) == std::optional<std::string>("{broken"),
           "failed repair Save keeps the draft and original bytes without reload");
  }
  expect(session.save([] { return true; }).saved(), "the explicit calibration repair saves durably");
  expect(session.undo() && session.document().visual_state() == platform::ModuleSidecarState::absent &&
             !session.document().has_unreadable_module() && session.redo(),
         "after commit undo is a normal editable removal, never a stale invalid flag");
  expect(!session.set_visual_runtime(core::VisualRuntime::isolated) &&
             session.document().profile().visual_runtime == core::VisualRuntime::isolated &&
             session.undo() && session.document().profile().visual_runtime == core::VisualRuntime::inline_process,
         "visual runtime choice is a validated base-profile history edit");
  expect(session.set_visual_runtime(static_cast<core::VisualRuntime>(255)).has_value(),
         "unknown visual runtime values are refused");
}

void duplicate_templates_retains_bytes_and_commits_atomically() {
  Fixture fixture; fixture.store(true);
  const auto &canonical = layerc::test::canonical_template_png_fixture();
  const std::array<std::uint8_t, 27> metadata{
      0, 0, 0, 15, 0x74, 0x45, 0x58, 0x74, 0x6f, 0x72, 0x69, 0x67, 0x69,
      0x6e, 0, 0x70, 0x6f, 0x72, 0x74, 0x61, 0x62, 0x6c, 0x65, 0x39, 0x6a, 0x79, 0x61};
  auto original = canonical.bytes;
  original.insert(original.end() - 12, metadata.begin(), metadata.end());
  const auto digest = core::sha256(std::as_bytes(std::span(original)));
  const auto first = platform::profile_visual_asset_path(fixture.path, canonical.sha256);
  const auto second = platform::profile_visual_asset_path(fixture.path, digest);
  std::filesystem::create_directories(first.parent_path());
  expect(layerc::test::write_file(first, canonical.bytes) &&
             layerc::test::write_file(second, original), "prepare two encoded identities");
  editor::EditorSession session(fixture.directory);
  expect(!session.open_path(fixture.path), "open the source bundle");
  expect(!session.edit_visual([&](auto &visual) -> std::optional<std::string> {
    core::TemplateState state; state.id = "ready"; state.name = "Ready";
    state.references = {{canonical.sha256}, {digest}};
    visual->observations.front() = state; return std::nullopt;
  }) && session.save([] { return true; }).saved(), "save valid template references");
  const auto source_visual = session.document().visual();
  std::filesystem::remove(second);
  expect(session.duplicate_current().has_value() && !session.is_draft() &&
             session.document().visual() == source_visual,
         "a missing winning reference refuses duplication without replacing the document");
  expect(layerc::test::write_file(second, original) && !session.duplicate_current() &&
             session.is_draft(), "a complete duplicate retains both encoded asset identities");
  // A second unsaved duplication must use owned bytes, not an empty source path.
  expect(!session.duplicate_current(), "a duplicate draft can itself be duplicated");
  const auto destination = editor::default_profile_path(fixture.directory, session.document().profile());
  const auto copied_first = platform::profile_visual_asset_path(destination, canonical.sha256);
  const auto copied_second = platform::profile_visual_asset_path(destination, digest);
  std::filesystem::remove_all(first.parent_path());
  std::filesystem::create_directories(copied_second); // Unsafe second target, after a valid first.
  const auto refused = session.save([] { return true; });
  expect(!refused.saved() && !refused.reload && session.is_draft() &&
             !std::filesystem::exists(destination) &&
             !std::filesystem::exists(platform::visual_sidecar_path(destination)) &&
             !std::filesystem::exists(copied_first),
         "refused copy Save publishes neither the bundle nor a prefix of the asset set");
  std::filesystem::remove(copied_second);
#ifdef _WIN32
  std::filesystem::create_directory(platform::visual_sidecar_path(destination));
#endif
  {
#ifndef _WIN32
    const auto scope = platform::profile_transaction_scope(fixture.directory);
    if (!expect(scope.has_value(), "resolve the private transaction scope")) return;
    platform::TransactionFileLock lock(*scope);
    expect(lock.locked(), "hold only the disposable store writer");
#endif
    const auto busy = session.save([] { return true; });
    expect(!busy.saved() && !busy.reload && !std::filesystem::exists(destination),
           "busy writer preserves the complete duplicate draft");
  }
#ifdef _WIN32
  std::filesystem::remove(platform::visual_sidecar_path(destination));
#endif
  expect(session.save([] { return true; }).saved() &&
             layerc::test::read_file(copied_first) == std::optional<std::string>(canonical.bytes) &&
             layerc::test::read_file(copied_second) == std::optional<std::string>(original),
         "retry atomically saves original encoded bytes independently of the deleted source");
  session.close_document();
  expect(!session.open_path(destination), "reopen the duplicate from its own namespace");
  for (const auto &reference : std::get<core::TemplateState>(
           session.document().visual()->observations.front()).references)
    expect(std::holds_alternative<core::TemplateImage>(
               platform::load_profile_template_asset(destination, reference.asset_sha256)),
           "every reopened template resolves without access to the source assets");
}

void calibration_assets_publish_with_bundle_and_survive_undo() {
  Fixture fixture; fixture.store(true);
  const auto &canonical = layerc::test::canonical_template_png_fixture();
  auto image = layerc::test::checker_template_image();
  image.asset_sha256 = canonical.sha256;
  editor::EditorSession session(fixture.directory);
  expect(!session.open_path(fixture.path), "open the calibration source");
  const auto asset = platform::profile_visual_asset_path(fixture.path, canonical.sha256);
  auto prepared = platform::prepare_profile_template_assets(fixture.path, std::span(&image, 1));
  auto *assets = std::get_if<std::vector<platform::EncodedTemplateAsset>>(&prepared);
  expect(assets && assets->size() == 1 && assets->front().asset_sha256 == canonical.sha256 &&
             !std::filesystem::exists(asset),
         "calibration images become owned canonical bytes without a store write");
  auto model = editor::CalibrationModel::open(session.document().profile(), "ability",
                                              session.document().visual());
  expect(model && model->add_reference({canonical.sha256}), "seed a template calibration");
  if (!model) return;
  auto controls = model->initial_controls();
  controls.predicate = editor::CalibrationPredicate::template_match;
  auto built = model->build_result(controls, std::vector<core::TemplateImage>{image});
  const auto *visual = std::get_if<core::VisualProfile>(&built);
  expect(visual != nullptr, "a template calibration builds one observation and trigger");
  if (!visual) return;
  const auto sidecar = platform::visual_sidecar_path(fixture.path);
  const auto before = layerc::test::read_file(sidecar);
  const auto update = [] { return editor::SharedObservationChoice::update_shared; };
  auto prepared_calibration =
      session.prepare_calibration("ability", *visual, std::span(&image, 1), update);
  auto *accepted_calibration =
      std::get_if<editor::AcceptedCalibration>(&prepared_calibration);
  expect(accepted_calibration != nullptr,
         "a typed template calibration prepares without publishing");
  expect(accepted_calibration &&
             !session.apply_calibration(std::move(*accepted_calibration)) &&
             session.owned_template_assets().size() == 1 && !std::filesystem::exists(asset),
         "applying a calibration owns its assets and still publishes nothing");
  expect(std::holds_alternative<core::TemplateImage>(platform::load_profile_template_asset(
             fixture.path, canonical.sha256, session.owned_template_assets())) &&
             !std::holds_alternative<core::TemplateImage>(
                 platform::load_profile_template_asset(fixture.path, canonical.sha256)),
         "a reopened calibration reads unpublished owned bytes before the store");
  expect(session.undo() && session.owned_template_assets().size() == 1,
         "an undone calibration keeps its bytes while redo can restore it");
  expect(!session.edit_visual([](auto &candidate) -> std::optional<std::string> {
           std::get<core::PixelRule>(candidate->observations.front()).name = "New branch";
           return std::nullopt;
         }) && session.owned_template_assets().empty(),
         "an edit that discards the redo branch releases unreachable owned bytes");
  auto again = session.prepare_calibration("ability", *visual, std::span(&image, 1), update);
  auto *accepted_again = std::get_if<editor::AcceptedCalibration>(&again);
  expect(accepted_again && !session.apply_calibration(std::move(*accepted_again)) &&
             session.owned_template_assets().size() == 1,
          "the calibration applies again with fresh owned bytes");
  {
#ifdef _WIN32
    platform::OwnedHandle lock(CreateFileW(sidecar.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!expect(lock.valid(), "prevent replacement of the disposable visual sidecar")) return;
#else
    const auto scope = platform::profile_transaction_scope(fixture.directory);
    if (!expect(scope.has_value(), "resolve the private transaction scope")) return;
    platform::TransactionFileLock lock(*scope);
    expect(lock.locked(), "hold only the disposable store writer");
#endif
    const auto busy = session.save([] { return true; });
    expect(!busy.saved() && !busy.reload && !std::filesystem::exists(asset) &&
               layerc::test::read_file(sidecar) == before && session.document().dirty() &&
               session.owned_template_assets().size() == 1,
           "a refused calibration Save publishes neither assets nor the bundle");
  }
  expect(session.save([] { return true; }).saved() &&
             layerc::test::read_file(asset) == std::optional<std::string>(canonical.bytes) &&
             layerc::test::read_file(sidecar) != before && session.owned_template_assets().empty(),
         "Save publishes the calibration assets with the profile and sidecar");

  expect(!session.duplicate_current() && session.is_draft() &&
             session.owned_template_assets().size() == 1,
         "a duplicate owns its source template bytes");
  expect(!session.edit_visual([](auto &candidate) -> std::optional<std::string> {
           candidate.reset();
           return std::nullopt;
         }), "remove the duplicate's visual module before its first Save");
  const auto destination = editor::default_profile_path(fixture.directory, session.document().profile());
  const auto copied = platform::profile_visual_asset_path(destination, canonical.sha256);
  expect(session.save([] { return true; }).saved() &&
#ifdef _WIN32
             copied == asset && layerc::test::read_file(copied) == std::optional<std::string>(canonical.bytes) &&
#else
             !std::filesystem::exists(copied) &&
#endif
             session.owned_template_assets().size() == 1,
         "an unreferenced owned asset stays owned after the first Save");
  std::filesystem::remove(asset);
  expect(session.undo() && session.document().visual() &&
             session.save([] { return true; }).saved() &&
             layerc::test::read_file(copied) == std::optional<std::string>(canonical.bytes) &&
             session.owned_template_assets().empty(),
         "Undo after Save restores a reference whose bytes the next Save publishes");
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
// Calibration is prepared once into a typed value: preparation is inert and
// the accepted value applies without asking the shared-choice dialog again.
void calibration_prepare_is_inert_and_accepts_once() {
  Fixture fixture;
  fixture.store();
  editor::EditorSession session(fixture.directory);
  expect(!session.open(0), "open the shared-observation calibration fixture");
  expect(!session.edit([](auto &profile) {
           auto second = profile.cooldowns.front();
           second.id = "other";
           second.name = "Other";
           profile.cooldowns.push_back(second);
         }),
         "add a cooldown that shares the observation");
  core::VisualProfile shared;
  shared.schema_version = 2;
  shared.profile_id = session.document().profile().id;
  core::PixelRule shared_rule;
  shared_rule.id = "shared";
  shared_rule.name = "Shared";
  shared_rule.region = core::NormalizedRegion{0.4, 0.4, 0.2, 0.2};
  shared.observations.push_back(shared_rule);
  shared.triggers.push_back({.cooldown_id = "ability", .observation_id = "shared"});
  shared.triggers.push_back({.cooldown_id = "other", .observation_id = "shared"});
  expect(!session.edit_visual([&](auto &visual) -> std::optional<std::string> {
           visual = shared;
           return std::nullopt;
         }),
         "install one observation referenced by two cooldowns");
  auto model = editor::CalibrationModel::open(session.document().profile(), "ability",
                                              session.document().visual());
  expect(model.has_value(), "seed the shared-observation calibration");
  auto built = model->build_result(model->initial_controls(), {});
  const auto *calibration = std::get_if<core::VisualProfile>(&built);
  if (!expect(calibration != nullptr,
              "the shared calibration builds one observation and trigger")) return;
  const auto before_profile = session.document().profile();
  const auto before_visual = session.document().visual();
  const auto before_generation = session.edit_generation();
  const auto before_undo = session.can_undo();
  const auto before_assets = session.owned_template_assets().size();
  const auto sidecar = platform::visual_sidecar_path(fixture.path);
  const auto before_bytes = layerc::test::read_file(sidecar);
  int choices{};
  const auto chooser = [&] {
    ++choices;
    return editor::SharedObservationChoice::clone;
  };
  auto prepared = session.prepare_calibration("ability", *calibration, {}, chooser);
  auto *accepted = std::get_if<editor::AcceptedCalibration>(&prepared);
  expect(accepted != nullptr && choices == 1,
         "preparation asks the shared choice exactly once");
  expect(session.document().profile() == before_profile &&
             session.document().visual() == before_visual &&
             session.edit_generation() == before_generation &&
             session.can_undo() == before_undo &&
             session.owned_template_assets().size() == before_assets &&
             layerc::test::read_file(sidecar) == before_bytes,
         "preparation changes no document, history, owned bytes or store file");
  const auto cancelled = session.prepare_calibration(
      "ability", *calibration, {},
      [] { return editor::SharedObservationChoice::cancel; });
  expect(std::get_if<std::string>(&cancelled) != nullptr &&
             session.document().profile() == before_profile &&
             session.document().visual() == before_visual &&
             session.edit_generation() == before_generation &&
             session.can_undo() == before_undo &&
             session.owned_template_assets().size() == before_assets &&
             layerc::test::read_file(sidecar) == before_bytes,
         "a cancelled shared choice changes no document, history, bytes or file");
  core::VisualProfile invalid;
  invalid.schema_version = 2;
  invalid.profile_id = session.document().profile().id;
  core::PixelRule invalid_rule;
  invalid_rule.id = "only";
  invalid_rule.name = "Only";
  invalid_rule.region = core::NormalizedRegion{0.4, 0.4, 0.2, 0.2};
  invalid.observations.push_back(invalid_rule);
  const auto rejected = session.prepare_calibration("ability", invalid, {}, chooser);
  expect(std::get_if<std::string>(&rejected) != nullptr && choices == 1 &&
             session.document().profile() == before_profile &&
             session.document().visual() == before_visual &&
             session.edit_generation() == before_generation &&
             session.owned_template_assets().size() == before_assets &&
             layerc::test::read_file(sidecar) == before_bytes,
         "an invalid typed result changes no document, history, bytes or file");
  auto retried = session.prepare_calibration("ability", *calibration, {}, chooser);
  expect(std::get_if<editor::AcceptedCalibration>(&retried) != nullptr && choices == 2,
         "retrying after a refusal prepares the same draft with a fresh choice");
  expect(accepted && !session.apply_calibration(std::move(*accepted)) && choices == 2 &&
             session.edit_generation() == before_generation + 1,
         "applying the accepted value asks nothing again and records one edit");
  const auto applied = session.document().visual();
  expect(applied && applied->observations.size() == 2 &&
             applied->triggers.size() == 2 &&
             layerc::core::find_observation(*applied, "shared") != nullptr &&
             layerc::core::find_observation(*applied, "shared-2") != nullptr,
         "cloning keeps the shared observation and adds this cooldown's own");
  expect(session.undo() && session.document().visual() == before_visual &&
             session.redo() && session.document().visual() == applied,
         "one undo and redo restore the whole calibration module");
}

// Assets stay owned until Save, and generation/path tokens reject stale values
// before any ownership, history or document change.
void calibration_assets_and_stale_tokens() {
  Fixture fixture;
  fixture.store(true);
  const auto &canonical = layerc::test::canonical_template_png_fixture();
  auto image = layerc::test::checker_template_image();
  image.asset_sha256 = canonical.sha256;
  editor::EditorSession session(fixture.directory);
  expect(!session.open_path(fixture.path), "open the calibration asset fixture");
  auto model = editor::CalibrationModel::open(session.document().profile(), "ability",
                                              session.document().visual());
  expect(model && model->add_reference({canonical.sha256}), "seed a template calibration");
  auto controls = model->initial_controls();
  controls.predicate = editor::CalibrationPredicate::template_match;
  auto built = model->build_result(controls, std::vector<core::TemplateImage>{image});
  const auto *visual = std::get_if<core::VisualProfile>(&built);
  if (!expect(visual != nullptr, "a template calibration builds one observation and trigger")) return;
  const auto asset = platform::profile_visual_asset_path(fixture.path, canonical.sha256);
  const auto generation = session.edit_generation();
  auto prepared = session.prepare_calibration("ability", *visual, std::span(&image, 1), {});
  auto *accepted = std::get_if<editor::AcceptedCalibration>(&prepared);
  expect(accepted != nullptr && accepted->assets.size() == 1 &&
             accepted->assets.front().asset_sha256 == canonical.sha256 &&
             session.owned_template_assets().empty() &&
             session.edit_generation() == generation && !std::filesystem::exists(asset),
         "preparation owns canonical bytes without publishing or mutating the document");
  auto missing = image;
  missing.asset_sha256 = std::string(64, 'f');
  const auto before_refusal_visual = session.document().visual();
  const auto refused_assets =
      session.prepare_calibration("ability", *visual, std::span(&missing, 1), {});
  expect(std::get_if<std::string>(&refused_assets) != nullptr &&
             session.owned_template_assets().empty() &&
             session.edit_generation() == generation &&
             session.document().visual() == before_refusal_visual,
         "an unavailable template asset refuses preparation without ownership");
  auto stale = session.prepare_calibration("ability", *visual, std::span(&image, 1), {});
  auto *stale_accepted = std::get_if<editor::AcceptedCalibration>(&stale);
  expect(stale_accepted != nullptr, "prepare a value that will become stale");
  expect(!session.edit([](auto &profile) { profile.name = "Edited after calibration"; }),
         "edit the document after preparing");
  const auto edited_generation = session.edit_generation();
  const auto edited_visual = session.document().visual();
  expect(stale_accepted &&
             session.apply_calibration(std::move(*stale_accepted)) ==
                 std::optional<std::string>(
                     "The active document changed during calibration.") &&
             session.edit_generation() == edited_generation &&
             session.document().visual() == edited_visual &&
             session.owned_template_assets().empty(),
         "a stale generation token is refused before any ownership or edit");
  auto closed = session.prepare_calibration("ability", *visual, std::span(&image, 1), {});
  auto *closed_accepted = std::get_if<editor::AcceptedCalibration>(&closed);
  session.close_document();
  expect(!session.open_path(fixture.path), "reopen the calibration document");
  expect(closed_accepted &&
             session.apply_calibration(std::move(*closed_accepted)) ==
                 std::optional<std::string>(
                     "The active document changed during calibration.") &&
             session.owned_template_assets().empty(),
         "a reopened document rejects the old generation token");
  auto drafted = session.prepare_calibration("ability", *visual, std::span(&image, 1), {});
  auto *drafted_accepted = std::get_if<editor::AcceptedCalibration>(&drafted);
  session.new_profile();
  expect(drafted_accepted &&
             session.apply_calibration(std::move(*drafted_accepted)) ==
                 std::optional<std::string>(
                     "The active document changed during calibration.") &&
             session.owned_template_assets().empty(),
         "a new draft rejects a token prepared for the saved path");
  expect(!session.open_path(fixture.path), "reopen the saved profile for the invalid result");
  auto corrupted = session.prepare_calibration("ability", *visual, std::span(&image, 1), {});
  auto *corrupted_accepted = std::get_if<editor::AcceptedCalibration>(&corrupted);
  if (!expect(corrupted_accepted != nullptr, "prepare a value to corrupt")) return;
  corrupted_accepted->visual.triggers.front().cooldown_id = "missing";
  const auto before_corrupt_generation = session.edit_generation();
  const auto before_corrupt_visual = session.document().visual();
  const auto error = session.apply_calibration(std::move(*corrupted_accepted));
  expect(error && !error->empty() &&
             session.edit_generation() == before_corrupt_generation &&
             session.document().visual() == before_corrupt_visual &&
             session.owned_template_assets().empty() &&
             !std::filesystem::exists(asset),
         "final validation refuses a corrupted accepted value before asset append");
  // An identical recalibration is a success, not an error: nothing changes and
  // no new step or bytes are adopted.
  auto applied_once =
      session.prepare_calibration("ability", *visual, std::span(&image, 1), {});
  auto *applied_once_accepted = std::get_if<editor::AcceptedCalibration>(&applied_once);
  if (!expect(applied_once_accepted != nullptr, "prepare the baseline recalibration")) return;
  expect(!session.apply_calibration(std::move(*applied_once_accepted)),
         "the baseline recalibration applies");
  auto again_prepared =
      session.prepare_calibration("ability", *visual, std::span(&image, 1), {});
  auto *again_accepted = std::get_if<editor::AcceptedCalibration>(&again_prepared);
  if (!expect(again_accepted != nullptr, "prepare the identical recalibration")) return;
  const auto before_identical_generation = session.edit_generation();
  const auto before_identical_visual = session.document().visual();
  const auto before_identical_assets = session.owned_template_assets().size();
  expect(!session.apply_calibration(std::move(*again_accepted)) &&
             session.edit_generation() == before_identical_generation &&
             session.document().visual() == before_identical_visual &&
             session.owned_template_assets().size() == before_identical_assets,
         "an identical recalibration succeeds without a step or new bytes");
}
} // namespace

// Every accepted transition notifies the listener exactly once, at the final
// state, with the generation already advanced; refusals never notify.
void change_listener_contracts() {
  Fixture fixture;
  fixture.store();
  editor::EditorSession session(fixture.directory);
  int notifications{};
  std::uint64_t observed_generation{};
  const auto listen = [&] {
    notifications = 0;
    observed_generation = 0;
    session.set_change_listener([&] {
      ++notifications;
      observed_generation = session.edit_generation();
    });
  };
  const auto advance = [&] { return session.edit_generation(); };
  listen();
  expect(!session.open(0), "open the stored fixture");
  expect(notifications == 1 && observed_generation == advance(),
         "open notifies once after the document is installed");
  const auto accept = [&](const std::function<void(core::GameProfile &)> &change) {
    notifications = 0;
    const auto outcome = session.edit(change);
    expect(!outcome && notifications == 1 && observed_generation == advance(),
           "an accepted base edit notifies once at the final state");
  };
  accept([](auto &profile) { profile.name = "Notified"; });
  const auto profile_id = session.document().profile().id;
  const auto visual_profile_id = profile_id;
  notifications = 0;
  expect(session.edit([](auto &profile) { profile.cooldowns.front().duration =
                                             core::Milliseconds(0); }).has_value() &&
             notifications == 0,
         "a refused edit notifies nobody");
  notifications = 0;
  expect(!session.edit_visual([visual_profile_id = profile_id](
                                  auto &visual) -> std::optional<std::string> {
           if (!visual) {
             core::PixelRule rule;
             rule.id = "ready";
             rule.name = "Ready";
             visual = core::VisualProfile{};
             visual->profile_id = visual_profile_id;
             visual->observations.push_back(rule);
           }
           std::get<core::PixelRule>(visual->observations.front()).name = "Notified rule";
           return std::nullopt;
         }).has_value() &&
             notifications == 1 && observed_generation == advance(),
         "an accepted visual edit notifies once");
  notifications = 0;
  expect(!session.edit_automation([profile_id = profile_id](
                                      auto &automation) -> std::optional<std::string> {
           core::AutomationDefinition definition;
           definition.id = "automation-1";
           if (!automation) automation = core::AutomationProfile{};
           automation->profile_id = profile_id;
           automation->automations.push_back(definition);
           return std::nullopt;
         }).has_value() &&
             notifications == 1 && observed_generation == advance(),
         "an accepted automation edit notifies once");
  notifications = 0;
  expect(!session.edit([](auto &profile) {
           profile.name = profile.name;
         }).has_value() &&
             notifications == 0,
         "an unchanged edit notifies nobody");
  notifications = 0;
  expect(session.undo() && notifications == 1 && observed_generation == advance(),
         "undo notifies once");
  notifications = 0;
  expect(session.redo() && notifications == 1 && observed_generation == advance(),
         "redo notifies once");
  const auto blocked =
      platform::recovery_backup_path(session.document().path());
  std::filesystem::remove_all(blocked);
  std::filesystem::create_directory(blocked);
  notifications = 0;
  expect(!session.save([] { return true; }).saved() && notifications == 0,
         "a failed save notifies nobody");
  std::filesystem::remove_all(blocked);
  notifications = 0;
  expect(session.save([] { return true; }).saved() && notifications == 1 &&
             observed_generation == advance(),
         "a successful save notifies once after rescan");
  notifications = 0;
  session.close_document();
  expect(notifications == 1 && observed_generation == advance(), "close notifies once");
  notifications = 0;
  session.new_profile();
  expect(notifications == 1 && observed_generation == advance(), "new notifies once");
  expect(!session.edit([](auto &profile) {
           profile.game.executable_name = "fixture.exe";
         }),
         "give the fresh draft a matching executable");
  notifications = 0;
  const auto duplicated = session.duplicate_current();
  expect(!duplicated.has_value() && notifications == 1 &&
             observed_generation == advance(),
         "duplicate notifies once: notifications=" + std::to_string(notifications) +
             " error=" + duplicated.value_or("-"));
  session.set_change_listener({});
  notifications = 0;
  session.close_document();
  session.new_profile();
  expect(notifications == 0, "a cleared listener is never called");
}

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(drafts_and_history),
                 LAYERC_TEST_CASE(mutation_candidates_and_preservation),
                 LAYERC_TEST_CASE(save_and_delete_failures),
                 LAYERC_TEST_CASE(enablement_and_invalid_modules),
                 LAYERC_TEST_CASE(reviewed_import_export),
                 LAYERC_TEST_CASE(change_listener_contracts),
                 LAYERC_TEST_CASE(import_destination_race_and_invalid_json),
                 LAYERC_TEST_CASE(automation_candidates_history_and_recovery),
                 LAYERC_TEST_CASE(visual_candidates_history_and_recovery),
                 LAYERC_TEST_CASE(duplicate_templates_retains_bytes_and_commits_atomically),
                 LAYERC_TEST_CASE(calibration_assets_publish_with_bundle_and_survive_undo),
                 LAYERC_TEST_CASE(calibration_prepare_is_inert_and_accepts_once),
                 LAYERC_TEST_CASE(calibration_assets_and_stale_tokens),
                 LAYERC_TEST_CASE(field_parser_contracts))
