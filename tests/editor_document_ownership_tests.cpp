#include "document_commit.hpp"
#include "profile_document.hpp"
#include "profile_edit.hpp"
#include "visual_calibration.hpp"
#include "test_support.hpp"

#include "layerc/platform/profile_bundle_load.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using layerc::core::GameProfile;
using layerc::editor::CommitRecord;
using layerc::editor::CommitResult;
using layerc::editor::DraftOrigin;
using layerc::editor::ModuleEditStatus;
using layerc::editor::ProfileBundle;
using layerc::editor::ProfileDocument;
using layerc::test::expect;

// An isolated XDG store so commits go through the real durable transaction.
class IsolatedStore {
public:
  IsolatedStore()
      : temp_("layerc-document-ownership"),
        profiles_("LAYERC_PROFILE_DIR", temp_.path() / "profiles"),
        home_("HOME", temp_.path()),
        config_("XDG_CONFIG_HOME", temp_.path() / "config"),
        data_("XDG_DATA_HOME", temp_.path() / "data") {
    directory_ = layerc::platform::profile_directory();
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    expect(!error && !directory_.empty(), "an isolated document store exists");
  }
  [[nodiscard]] const std::filesystem::path &directory() const noexcept {
    return directory_;
  }

private:
  layerc::test::TempDirectory temp_;
  layerc::test::ScopedEnvironment profiles_, home_, config_, data_;
  std::filesystem::path directory_;
};

layerc::platform::ProfileBundleLoad loaded(const GameProfile &profile) {
  layerc::platform::ProfileBundleLoad bundle;
  bundle.profile = profile;
  return bundle;
}

GameProfile fixture_profile() {
  auto profile = layerc::editor::make_new_profile();
  profile.game.executable_name = "ownership.exe";
  return profile;
}

// 1. Saved-content equivalence: a new edit that restores the saved values is
// clean, exactly like an undo path back to them.
void saved_content_equivalence() {
  const auto saved = fixture_profile();
  ProfileDocument document;
  document.open("/disposable/saved.json", loaded(saved));
  expect(!document.dirty() && !document.can_undo(),
         "an opened document is clean with no history");
  expect(document.edit_profile([&](GameProfile &candidate) {
           candidate.name = "Renamed";
         }, layerc::editor::DocumentChangeKind::immediate).accepted() &&
             document.dirty() && document.can_undo(),
         "one accepted edit is dirty and one undo step");
  expect(document.edit_profile([&](GameProfile &candidate) {
           candidate.name = saved.name;
         }, layerc::editor::DocumentChangeKind::immediate).accepted() &&
             !document.dirty(),
         "a new edit restoring the saved content is clean");
  expect(document.undo() && document.dirty(),
         "undoing the restoring edit is dirty again");
  expect(document.undo() && !document.dirty(),
         "an undo path back to the saved content is clean");
}

// 2. Draft origins.
void draft_origins() {
  const auto profile = fixture_profile();
  ProfileDocument fresh;
  fresh.start_draft(profile, DraftOrigin::fresh);
  expect(!fresh.dirty() && !fresh.can_undo() && fresh.path().empty(),
         "a fresh draft starts clean and detached");
  expect(fresh.edit_profile([&](GameProfile &candidate) {
           candidate.name = "Edited";
         }, layerc::editor::DocumentChangeKind::text).accepted() &&
             fresh.dirty(),
         "editing a fresh draft makes it dirty");
  expect(fresh.edit_profile([&](GameProfile &candidate) {
           candidate.name = profile.name;
         }, layerc::editor::DocumentChangeKind::text).accepted() &&
             fresh.dirty(),
         "editing a fresh draft back to its start stays dirty (a draft edit forgets the saved snapshot)");
  expect(fresh.undo() && fresh.dirty(),
         "undoing a draft edit stays dirty until the draft is committed");

  ProfileDocument copy;
  copy.start_draft(profile, DraftOrigin::copy);
  expect(copy.dirty() && !copy.can_undo(),
         "an untouched copy draft is dirty by construction");
  auto bundle = copy.bundle();
  copy.accept_commit("/disposable/copy.json", std::move(bundle));
  expect(!copy.dirty() && copy.path() == std::filesystem::path("/disposable/copy.json") &&
             !copy.legacy_base_schema() && !copy.legacy_visual_schema() &&
             !copy.upgrade_acknowledged(),
         "a committed copy is clean, attached and in the current schema");
}

// 3. Calibration repair round trip over an unreadable visual module.
void calibration_repair_round_trip() {
  const auto profile = fixture_profile();
  auto bundle = loaded(profile);
  bundle.modules.get<layerc::core::VisualModule>() =
      layerc::platform::VisualSidecarLoad::invalid(
          {.path = "$", .message = "fixture invalid sidecar"}, {});
  ProfileDocument document;
  document.open("/disposable/unreadable.json", std::move(bundle));
  const auto diagnostic = document.visual_error();
  expect(document.visual_state() == layerc::platform::ModuleSidecarState::invalid &&
             !document.visual() && !document.dirty(),
         "an unreadable visual module opens preserved and clean");
  expect(document.edit_visual([](auto &visual) -> std::optional<std::string> {
           visual = layerc::core::VisualProfile{};
           return std::nullopt;
         }, layerc::editor::DocumentChangeKind::immediate).status ==
             ModuleEditStatus::blocked,
         "ordinary editing cannot replace the unreadable module");

  auto model = layerc::editor::CalibrationModel::open(profile, profile.cooldowns.front().id,
                                                      std::nullopt);
  if (!expect(model.has_value(), "the calibration model seeds")) return;
  auto built = model->build_result(model->initial_controls(), {});
  const auto *repaired = std::get_if<layerc::core::VisualProfile>(&built);
  if (!expect(repaired != nullptr, "the calibration builds a typed visual")) return;
  expect(document.replace_visual_from_calibration(*repaired).accepted() &&
             document.visual_state() == layerc::platform::ModuleSidecarState::valid &&
             document.dirty() && document.can_undo(),
         "an explicit calibration repairs the module in one step");
  expect(document.undo() && !document.visual() &&
             document.visual_state() == layerc::platform::ModuleSidecarState::invalid &&
             document.visual_error() == diagnostic,
         "undo restores the original unreadable state and diagnostic");
  expect(document.redo() && document.visual() && document.visual() == *repaired,
         "redo restores the repaired module");
}

// 4/5. Commit refusals are inert; a successful candidate commit is one step.
void commit_refusals_and_candidate() {
  const IsolatedStore store;
  const auto profile = fixture_profile();
  ProfileDocument document;
  // First save through the real store so the document owns a stored path.
  document.start_draft(profile, DraftOrigin::fresh);
  const auto first_path = layerc::editor::default_profile_path(store.directory(), profile);
  const auto first = layerc::editor::commit_document(document, first_path, {}, nullptr);
  expect(first.ok() && document.path() == first_path && !document.dirty(),
         "the ownership fixture saves once through the durable store");
  document.edit_profile([](GameProfile &candidate) { candidate.name = "Pending"; },
                        layerc::editor::DocumentChangeKind::immediate);
  const auto before = document.bundle();
  const auto before_path = document.path();
  const auto *cooldowns = document.profile().cooldowns.data();
  expect(document.dirty() && document.can_undo(), "the fixture has one pending edit");
  const auto refused = layerc::editor::commit_document(document, {}, {}, nullptr);
  expect(refused.result == CommitResult::write_failed && document.profile() == before.profile &&
             document.path() == before_path && document.dirty() &&
             document.profile().cooldowns.data() == cooldowns,
         "a failed commit leaves values, storage, path and dirty untouched");
  layerc::platform::ProfileBundleLoad legacy;
  legacy.profile = profile;
  legacy.legacy_base_schema = true;
  ProfileDocument legacy_document;
  legacy_document.open(store.directory() / "legacy.json", std::move(legacy));
  const auto before_undo = legacy_document.can_undo();
  const auto upgrade = layerc::editor::commit_document(
      legacy_document, store.directory() / "legacy.json", {}, [] { return false; });
  expect(upgrade.result == CommitResult::upgrade_declined &&
             legacy_document.can_undo() == before_undo && !legacy_document.dirty(),
         "a declined upgrade records no step and stays clean");

  const auto candidate = layerc::editor::commit_document(
      document, document.path(), {}, nullptr, layerc::platform::KeepModule{},
      layerc::platform::ProfileMutationPlanner{},
      [&](ProfileBundle &bundle) { bundle.profile.name = "Candidate"; });
  expect(candidate.ok() && !document.dirty() &&
             document.profile().name == "Candidate" &&
             document.path() == before_path && document.can_undo(),
         "a successful candidate commit records one step and rebases (result " +
             std::to_string(static_cast<int>(candidate.result)) + ", write_error " +
             std::to_string(candidate.write_error
                                ? static_cast<int>(*candidate.write_error)
                                : -1) + ")");
  expect(document.undo() && document.dirty() &&
             document.profile().name != "Candidate",
         "undoing a candidate commit is dirty again");
  const auto after_undo = document.profile();
  const auto failed_candidate = layerc::editor::commit_document(
      document, {}, {}, nullptr, layerc::platform::KeepModule{},
      layerc::platform::ProfileMutationPlanner{},
      [&](ProfileBundle &bundle) { bundle.profile.name = "Never"; });
  expect(failed_candidate.result == CommitResult::write_failed &&
             document.profile() == after_undo && document.dirty(),
         "a failed candidate commit leaves no step and no change (result " +
             std::to_string(static_cast<int>(failed_candidate.result)) + ")");
}

} // namespace

// The seal is a compile-time property: const readers only, no dirty setter.
static_assert(std::is_same_v<decltype(std::declval<ProfileDocument &>().profile()),
                             const layerc::core::GameProfile &>);
static_assert(std::is_same_v<decltype(std::declval<ProfileDocument &>().visual()),
                             const std::optional<layerc::core::VisualProfile> &>);
static_assert(std::is_same_v<decltype(std::declval<ProfileDocument &>().automation()),
                             const std::optional<layerc::core::AutomationProfile> &>);
static_assert(std::is_same_v<decltype(std::declval<ProfileDocument &>().path()),
                             const std::filesystem::path &>);
static_assert(std::is_same_v<decltype(std::declval<ProfileDocument &>().history()),
                             const layerc::editor::EditorProfileHistory &>);
template <class T> concept HasDirtySetter = requires(T &document) { document.set_dirty(true); };
static_assert(!HasDirtySetter<ProfileDocument>);

void editor_document_ownership_contracts() {
  saved_content_equivalence();
  draft_origins();
  calibration_repair_round_trip();
  commit_refusals_and_candidate();
}
