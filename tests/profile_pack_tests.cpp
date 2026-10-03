#include "layerc/bundle/profile_pack.hpp"
#include "../src/bundle/pack_detail.hpp"

#include "layerc/core/json.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"

#include "miniz.h"
#include "template_png_fixture.hpp"
#include "test_support.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <variant>
#include <vector>

namespace {

using layerc::test::expect;
using layerc::test::read_file;
using layerc::test::write_file;

// Never broadcast to a running daemon from a disposable bundle test.
layerc::bundle::PackOperationResult import_without_reload(
    const layerc::bundle::ValidatedPack &pack,
    const std::span<const layerc::bundle::ImportChoice> choices,
    const std::filesystem::path &directory) {
  const auto result = layerc::bundle::detail::commit_profile_import(pack, choices, directory);
  if (const auto *error = std::get_if<layerc::bundle::PackError>(&result)) return *error;
  return std::monostate{};
}

layerc::core::GameProfile profile(std::string id, std::string name,
                                  const std::uint16_t key) {
  layerc::core::GameProfile result;
  result.id = std::move(id);
  result.name = std::move(name);
  result.game.executable_name = "game.exe";
  result.cooldowns.push_back(
      {.id = "dash", .name = "Dash", .duration = std::chrono::seconds(5)});
  result.panels.push_back({.id = "dash-bar",
                           .name = "Dash",
                           .style = layerc::core::PanelStyle::bar,
                           .bar = {},
                           .cooldown_ids = {"dash"}});
  result.bindings.push_back(
      {.cooldown_id = "dash",
       .chord = {.tokens = {{.device = layerc::core::InputDevice::keyboard,
                             .code = key}}}});
  return result;
}

layerc::core::AutomationProfile automation_profile(const std::string &id) {
  using namespace layerc::core;
  AutomationProfile result;
  result.profile_id = id;
  result.variables.push_back({.id = "count",
                              .type = AutomationValueType::integer,
                              .default_value = AutomationValue(std::int64_t{0}),
                              .persistent = true});
  AutomationRule rule;
  rule.id = "start";
  rule.event = AutomationEventKind::session_started;
  AutomationAction action;
  action.kind = AutomationActionKind::increment_variable;
  action.target = "count";
  action.value = expr::literal(AutomationValue(std::int64_t{1}));
  rule.actions.push_back(std::move(action));
  result.automations.push_back(
      {.id = "counter", .name = "Counter", .rules = {rule}});
  return result;
}

bool write_traversal_zip(const std::filesystem::path &path) {
  mz_zip_archive zip{};
  if (!mz_zip_writer_init_heap(&zip, 0, 1024))
    return false;
  const std::string payload = "{}";
  bool ok = mz_zip_writer_add_mem(&zip, "../manifest.json", payload.data(),
                                  payload.size(), MZ_BEST_COMPRESSION) != 0;
  void *data{};
  size_t size{};
  ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &data, &size);
  mz_zip_writer_end(&zip);
  if (!ok || data == nullptr)
    return false;
  const bool written = layerc::test::write_file(
      path, std::string_view(static_cast<const char *>(data), size));
  mz_free(data);
  return written;
}

struct ZipRewrite {
  std::optional<std::string> replace_path{};
  std::string replacement{};
  std::optional<std::string> skip_path{};
  std::vector<std::pair<std::string, std::string>> extras{};
};

bool rewrite_zip(const std::filesystem::path &source,
                 const std::filesystem::path &destination,
                 const ZipRewrite &change) {
  mz_zip_archive reader{};
  mz_zip_archive writer{};
  if (!mz_zip_reader_init_file(&reader, source.string().c_str(), 0) ||
      !mz_zip_writer_init_heap(&writer, 0, 1024)) {
    mz_zip_reader_end(&reader);
    return false;
  }
  bool ok = true;
  const auto count = mz_zip_reader_get_num_files(&reader);
  for (mz_uint index = 0; ok && index < count; ++index) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&reader, index, &stat) ||
        stat.m_is_directory) {
      ok = false;
      break;
    }
    const std::string path = stat.m_filename;
    if (change.skip_path && path == *change.skip_path)
      continue;
    if (change.replace_path && path == *change.replace_path) {
      ok = mz_zip_writer_add_mem(
               &writer, path.c_str(), change.replacement.data(),
               change.replacement.size(), MZ_BEST_COMPRESSION) != 0;
      continue;
    }
    size_t size{};
    void *data = mz_zip_reader_extract_to_heap(&reader, index, &size, 0);
    ok = data != nullptr &&
         mz_zip_writer_add_mem(&writer, path.c_str(), data, size,
                               MZ_BEST_COMPRESSION) != 0;
    mz_free(data);
  }
  for (const auto &[path, payload] : change.extras) {
    ok = ok && mz_zip_writer_add_mem(&writer, path.c_str(), payload.data(),
                                     payload.size(), MZ_BEST_COMPRESSION) != 0;
  }
  void *archive_data{};
  size_t archive_size{};
  ok = ok && mz_zip_writer_finalize_heap_archive(&writer, &archive_data,
                                                 &archive_size) != 0;
  mz_zip_writer_end(&writer);
  mz_zip_reader_end(&reader);
  if (!ok || archive_data == nullptr) {
    mz_free(archive_data);
    return false;
  }
  const bool written = layerc::test::write_file(
      destination,
      std::string_view(static_cast<const char *>(archive_data), archive_size));
  mz_free(archive_data);
  return written;
}

const std::string &asset_hash = layerc::test::canonical_template_png_fixture().sha256;
const std::string &asset_png = layerc::test::canonical_template_png_fixture().bytes;

struct PackFixture {
  layerc::test::TempDirectory directory{"layerc-pack-tests"};
  std::filesystem::path root = directory.path();
  std::filesystem::path source = root / "source";
  std::filesystem::path imported = root / "imported";
  layerc::test::ScopedEnvironment profile_dir{"LAYERC_PROFILE_DIR", source};
  std::filesystem::path first_path = source / "first.json";
  std::filesystem::path second_path = source / "second.json";
  layerc::core::GameProfile first_base = profile("first", "First", 30);
  layerc::core::AutomationProfile first_automation = automation_profile(first_base.id);
  layerc::core::CompanionProfile first_companion;
  bool ready{};

  PackFixture() {
    std::error_code error;
    std::filesystem::create_directories(source, error);
    std::filesystem::create_directories(imported, error);
    if (!expect(!error, "profile-pack temporary directories should be created") ||
        !expect(profile_dir.applied(), "profile-pack store should use the fixture directory"))
      return;

    layerc::core::VisualProfile first_visual;
    first_visual.profile_id = "first";
    first_visual.observations.push_back(layerc::core::TemplateState{
        .id = "ready-template",
        .name = "Ready",
        .search_region = {.x = 0.1, .y = 0.1, .width = 0.4, .height = 0.4},
        .expected_width = 0.2,
        .expected_height = 0.2,
        .references = {{.asset_sha256 = std::string(asset_hash)}},
    });
    first_visual.triggers.push_back(
        {.cooldown_id = "dash", .observation_id = "ready-template"});
    bool passed = expect(write_file(source / "_visual" / "assets" /
                                        (std::string(asset_hash) + ".png"),
                                    asset_png),
                         "content-addressed test asset should be created");
    first_companion.profile_id = first_base.id;
    first_companion.slots.push_back({.slot_index = 0,
                                     .panel_id = "dash-bar",
                                     .pinned = true,
                                     .click_through = false,
                                     .opacity = 0.75});
    passed &= expect(layerc::platform::write_profile_bundle(
                         first_path, first_base, first_visual, first_automation,
                         first_companion).ok(),
                     "first schema-4 profile and all module sidecars should save");
    passed &= expect(
        layerc::platform::write_automation_state(
            first_path, first_automation,
            {{"count", layerc::core::AutomationValue(std::int64_t{9})}}).ok(),
        "machine-local automation state should exist beside the export fixture");
    passed &= expect(layerc::platform::write_profile_bundle(
                         second_path, profile("second", "Second", 31), std::nullopt).ok(),
                     "second schema-3 profile should save");
    ready = passed;
  }

  [[nodiscard]] bool export_pack(const std::filesystem::path &path) const {
    const std::array selected{second_path, first_path};
    return expect(std::holds_alternative<std::monostate>(
                      layerc::bundle::export_profile_pack(path, selected)),
                  "multi-profile pack should export");
  }

  [[nodiscard]] std::optional<layerc::bundle::ValidatedPack>
  validate_pack(const std::filesystem::path &path) const {
    const auto result = layerc::bundle::validate_profile_pack(path);
    const auto *validated = std::get_if<layerc::bundle::ValidatedPack>(&result);
    expect(validated != nullptr && validated->profiles.size() == 2,
           "exported pack should validate with both profiles");
    if (validated == nullptr || validated->profiles.size() != 2)
      return std::nullopt;
    return *validated;
  }

  [[nodiscard]] bool import_both(const layerc::bundle::ValidatedPack &pack) const {
    const std::array choices{
        layerc::bundle::ImportChoice{.profile_index = 0,
                                     .action = layerc::bundle::ImportAction::replace},
        layerc::bundle::ImportChoice{.profile_index = 1,
                                     .action = layerc::bundle::ImportAction::replace}};
    return expect(std::holds_alternative<std::monostate>(
                      import_without_reload(pack, choices, imported)),
                  "validated profiles should import atomically");
  }
};

void legacy_visual_schema_validation() {
  PackFixture fixture;
  if (!fixture.ready) return;
  const auto &root = fixture.root;
  const auto &source = fixture.source;
  const auto legacy_path = source / "legacy.json";
  auto legacy_base = profile("legacy", "Legacy", 32);
  legacy_base.schema_version = 3;
  legacy_base.modules.push_back(
      {.kind = layerc::core::ProfileModuleKind::visual, .schema_version = 1});
  constexpr std::string_view legacy_visual = R"json({
    "schemaVersion":1,"profileId":"legacy","sampleRateHz":10,
    "quality":"balanced","rules":[{
      "id":"ready","name":"Ready","region":{"x":0,"y":0,"width":1,"height":1},
      "predicate":{"type":"brightness","comparison":"above","threshold":100},
      "stableSamples":2,"repeat":"edge"}],
    "triggers":[{"cooldownId":"dash","ruleId":"ready","enabled":true,
      "mode":"pixelOnly","keepInput":false,"onUnavailable":"ignore"}]
  })json";
  if (!expect(write_file(legacy_path,
                         layerc::core::serialize_profile_json(legacy_base)) &&
                  write_file(layerc::platform::visual_sidecar_path(legacy_path),
                             legacy_visual),
              "matching schema-1 base metadata and visual sidecar should be staged"))
    return;
  const std::array legacy_selection{legacy_path};
  const auto legacy_pack = root / "legacy.layercprofile";
  expect(std::holds_alternative<std::monostate>(
             layerc::bundle::export_profile_pack(legacy_pack, legacy_selection)) &&
             std::holds_alternative<layerc::bundle::ValidatedPack>(
                 layerc::bundle::validate_profile_pack(legacy_pack)),
         "matching schema-1 module metadata should validate against the packed "
         "source schema before in-memory migration");

  legacy_base.modules.front().schema_version = 2;
  if (!expect(write_file(legacy_path,
                         layerc::core::serialize_profile_json(legacy_base)),
              "mismatched visual module metadata fixture should be staged"))
    return;
  const auto mismatched_schema_pack = root / "mismatched-schema.layercprofile";
  expect(std::holds_alternative<std::monostate>(
             layerc::bundle::export_profile_pack(mismatched_schema_pack,
                                                 legacy_selection)) &&
             std::holds_alternative<layerc::bundle::PackError>(
                 layerc::bundle::validate_profile_pack(mismatched_schema_pack)),
         "base module metadata must match the actual packed visual schema");
}

void deterministic_export_and_import() {
  PackFixture fixture;
  if (!fixture.ready) return;
  const auto &root = fixture.root;
  const auto &imported = fixture.imported;
  const auto pack_one = root / "one.layercprofile";
  const auto pack_two = root / "two.layercprofile";
  if (!fixture.export_pack(pack_one)) return;
  const std::array selected{fixture.second_path, fixture.first_path};
  if (!expect(std::holds_alternative<std::monostate>(
                  layerc::bundle::export_profile_pack(pack_two, selected)),
              "same multi-profile pack should export twice"))
    return;
  expect(read_file(pack_one) == read_file(pack_two),
         "pack export should be byte-for-byte deterministic");
  const auto nested_name = layerc::bundle::resolve_suggested_profile_filename(
      "folder/existing.json");
  expect(nested_name.has_value() && nested_name->wstring() == L"existing.json",
         "pack preview and import should resolve nested suggestions "
         "to one top-level basename");

  auto validated = fixture.validate_pack(pack_one);
  if (!validated) return;
  const auto first =
      std::ranges::find(validated->profiles, std::string("first"),
                        [](const auto &entry) { return entry.profile.id; });
  expect(first != validated->profiles.end() &&
             first->module<layerc::core::AutomationModule>().has_value() &&
             first->module<layerc::core::CompanionModule>().has_value() &&
             first->module<layerc::core::CompanionModule>()->profile ==
                 fixture.first_companion,
         "automation and companion definitions should be included "
         "in a profile pack");
  if (!fixture.import_both(*validated)) return;
  expect(std::filesystem::exists(imported / "first.json") &&
             std::filesystem::exists(imported / "second.json"),
         "suggested filenames should be retained without conflicts");
  expect(std::filesystem::exists(imported / "_visual" / "first.json") &&
             std::filesystem::exists(imported / "_visual" / "assets" /
                                     (std::string(asset_hash) + ".png")) &&
             read_file(imported / "_visual" / "assets" /
                       (std::string(asset_hash) + ".png")) == asset_png,
         "sidecars and assets should import beneath the requested "
         "destination root");
  expect(std::filesystem::exists(imported / "_automation" / "first.json") &&
             !std::filesystem::exists(imported / "_automation_state" /
                                      "first.json.automation-state.json"),
         "packs should import automation definitions while excluding "
         "machine-local state");

  const auto imported_companion = layerc::platform::load_companion_sidecar(
      imported / "first.json", fixture.first_base);
  expect(imported_companion.state() ==
                 layerc::platform::ModuleSidecarState::valid &&
             imported_companion.usable() &&
             *imported_companion.profile() == fixture.first_companion,
         "packs should import companion preferences with the profile bundle");
}

void copy_import_guards() {
  PackFixture fixture;
  if (!fixture.ready) return;
  const auto pack = fixture.root / "one.layercprofile";
  if (!fixture.export_pack(pack)) return;
  auto validated = fixture.validate_pack(pack);
  if (!validated || !fixture.import_both(*validated)) return;
  const auto &imported = fixture.imported;
  const auto first_to_copy =
      std::ranges::find(validated->profiles, std::string("first"),
                        [](const auto &entry) { return entry.profile.id; });
  if (!expect(first_to_copy != validated->profiles.end(),
              "the companion fixture should remain available for Copy"))
    return;
  const auto first_index = static_cast<std::size_t>(
      std::distance(validated->profiles.begin(), first_to_copy));
  const std::array copy_choice{layerc::bundle::ImportChoice{
      .profile_index = first_index, .action = layerc::bundle::ImportAction::copy}};
  expect(std::holds_alternative<std::monostate>(
             import_without_reload(*validated, copy_choice, imported)),
         "Import Copy should succeed beside existing profiles");
  const auto copy_preview = layerc::bundle::preview_profile_import(
      *validated, copy_choice, imported);
  expect(!copy_preview.error && copy_preview.entries.size() == 1,
         "Copy preview should resolve the next available destination");
  if (!copy_preview.error && copy_preview.entries.size() == 1) {
    const auto target = copy_preview.entries.front().target;
    auto reviewed_choice = copy_choice;
    reviewed_choice.front().expected_target = target;
    reviewed_choice.front().require_absent = true;
    if (expect(write_file(target, "new occupant"),
               "a destination can become occupied after preview")) {
      expect(std::holds_alternative<layerc::bundle::PackError>(
                 import_without_reload(*validated, reviewed_choice, imported)) &&
                 read_file(target) == "new occupant",
             "a changed Copy destination must fail without overwriting it");
    }
    std::error_code error;
    std::filesystem::remove(target, error);
  }
  bool found_copy{};
  bool found_companion_copy{};
  for (const auto &entry : std::filesystem::directory_iterator(imported)) {
    if (entry.path().extension() != ".json")
      continue;
    const auto json = layerc::platform::read_profile_file(entry.path());
    if (!json.has_value())
      continue;
    const auto parsed = layerc::core::parse_profile_json(*json);
    const auto *candidate = std::get_if<layerc::core::GameProfile>(&parsed);
    if (candidate == nullptr ||
        candidate->id.find("-copy") == std::string::npos)
      continue;
    found_copy = true;
    const auto copied_companion =
        layerc::platform::load_companion_sidecar(entry.path(), *candidate);
    found_companion_copy =
        copied_companion.state() ==
            layerc::platform::ModuleSidecarState::valid &&
        copied_companion.usable() &&
        copied_companion.profile()->profile_id == candidate->id &&
        copied_companion.profile()->slots.size() == 1U &&
        copied_companion.profile()->slots.front().panel_id == "dash-bar";
  }
  expect(found_copy && found_companion_copy,
         "Import Copy should regenerate both base and companion profile IDs");
}

void import_preview_conflicts() {
  PackFixture fixture;
  if (!fixture.ready) return;
  const auto pack = fixture.root / "one.layercprofile";
  if (!fixture.export_pack(pack)) return;
  auto validated = fixture.validate_pack(pack);
  if (!validated) return;
  const auto preview_root = fixture.root / "preview-must-not-create";
  std::vector<layerc::bundle::ImportChoice> replace_all;
  for (std::size_t index = 0; index < validated->profiles.size(); ++index)
    replace_all.push_back({.profile_index = index,
                           .action = layerc::bundle::ImportAction::replace});
  const auto preview = layerc::bundle::preview_profile_import(
      *validated, replace_all, preview_root);
  expect(!preview.error && !preview.entries.empty() &&
             !std::filesystem::exists(preview_root),
         "import preview must resolve destinations without creating the store");
  const auto conflict_root = fixture.root / "conflicts";
  const auto invalid_target = conflict_root / "existing.JSON";
  if (!expect(write_file(invalid_target, "{invalid-profile"),
              "invalid conflict fixture should be created"))
    return;
  auto nested_pack = *validated;
  nested_pack.profiles.front().suggested_filename = "folder/existing.json";
  const auto invalid_preview = layerc::bundle::preview_profile_import(
      nested_pack, replace_all, conflict_root);
  expect(invalid_preview.error.has_value(),
         "preview must reject the same unreadable filename collision as commit");
  const std::array replace_nested{layerc::bundle::ImportChoice{
      .profile_index = 0, .action = layerc::bundle::ImportAction::replace}};
  expect(std::holds_alternative<layerc::bundle::PackError>(
             import_without_reload(nested_pack, replace_nested, conflict_root)) &&
             read_file(invalid_target) == "{invalid-profile",
         "Replace must reject a case-colliding unreadable JSON "
         "target instead of overwriting it without a Copy choice");
}

void copy_original_id_reservations() {
  layerc::test::TempDirectory temp("LayerC-copy-original-ids");
  layerc::test::ScopedEnvironment profile_dir("LAYERC_PROFILE_DIR", temp.path());
  if (!expect(profile_dir.applied(), "copy allocation must use a disposable store")) return;
  layerc::bundle::ValidatedPack pack;
  for (const auto &[id, filename] : std::array{
           std::pair{"foo", "foo.json"}, std::pair{"foo-copy", "foo-copy.json"}}) {
    auto base = profile(id, id, 30);
    pack.profiles.push_back({.suggested_filename = filename,
                            .base_json = layerc::core::serialize_profile_json(base),
                            .profile = std::move(base)});
  }
  const std::array choices{
      layerc::bundle::ImportChoice{.profile_index = 0,
                                  .action = layerc::bundle::ImportAction::copy},
      layerc::bundle::ImportChoice{.profile_index = 1,
                                  .action = layerc::bundle::ImportAction::replace}};
  const std::array reversed{choices[1], choices[0]};
  const auto check_orders = [&](const std::filesystem::path &expected_copy) {
    const auto forward = layerc::bundle::preview_profile_import(pack, choices, temp.path());
    const auto backward = layerc::bundle::preview_profile_import(pack, reversed, temp.path());
    if (!expect(!forward.error && !backward.error &&
                    forward.entries.size() == 2 && backward.entries.size() == 2,
                "copy plus selected original should have valid previews in either order")) return;
    expect(forward.entries[0].id == "foo-copy-2" &&
               forward.entries[1].id == "foo-copy" &&
               backward.entries[0].id == forward.entries[1].id &&
               backward.entries[1].id == forward.entries[0].id &&
               forward.entries[0].target == expected_copy &&
               backward.entries[1].target == forward.entries[0].target &&
               backward.entries[0].target == forward.entries[1].target,
           "Copy(foo) must reserve Replace(foo-copy)'s ID independently of choice order");
    expect(!layerc::bundle::validate_import_choices(pack, forward.choices, temp.path()) &&
               !layerc::bundle::validate_import_choices(pack, backward.choices, temp.path()),
           "complete final copy/original identities and targets should validate without publication");
  };
  check_orders(temp.path() / "foo copy.json");
  pack.profiles[1].suggested_filename = "foo copy.json";
  check_orders(temp.path() / "foo copy 2.json");
  std::error_code error;
  expect(std::filesystem::is_empty(temp.path(), error) && !error,
         "copy allocation preview/validation must not write a store or broadcast a reload");
}

void copy_choice_order_is_canonical() {
  layerc::test::TempDirectory temp("LayerC-canonical-copies");
  layerc::test::ScopedEnvironment profile_dir("LAYERC_PROFILE_DIR", temp.path());
  if (!expect(profile_dir.applied(), "copy ordering must use a disposable store")) return;
  layerc::bundle::ValidatedPack pack;
  for (const auto id : {"foo", "foo-copy"}) {
    auto base = profile(id, id, 30);
    pack.profiles.push_back({.suggested_filename = "foo.json",
                            .base_json = layerc::core::serialize_profile_json(base),
                            .profile = std::move(base)});
  }
  const std::array choices{
      layerc::bundle::ImportChoice{.profile_index = 0,
                                  .action = layerc::bundle::ImportAction::copy},
      layerc::bundle::ImportChoice{.profile_index = 1,
                                  .action = layerc::bundle::ImportAction::copy}};
  const std::array reversed{choices[1], choices[0]};
  const auto forward = layerc::bundle::preview_profile_import(pack, choices, temp.path());
  const auto backward = layerc::bundle::preview_profile_import(pack, reversed, temp.path());
  expect(!forward.error && !backward.error &&
             forward.entries.size() == 2 && backward.entries.size() == 2 &&
             forward.entries[0].id == backward.entries[1].id &&
             forward.entries[1].id == backward.entries[0].id &&
             forward.entries[0].target == backward.entries[1].target &&
             forward.entries[1].target == backward.entries[0].target,
         "multiple copy identities and destinations must be canonical across choice permutations");
}

void import_planning_after_recovery() {
  layerc::test::TempDirectory temp("LayerC-import-owned-plan");
  layerc::test::ScopedEnvironment profile_dir("LAYERC_PROFILE_DIR", temp.path());
  if (!expect(profile_dir.applied(), "import planning must use a disposable store")) return;
  const auto target = temp.path() / "a.json";
  auto preceding_base = profile("foo", "Last committed", 30);
  layerc::core::CompanionProfile preceding_companion;
  preceding_companion.profile_id = preceding_base.id;
  preceding_companion.slots.push_back(
      {.slot_index = 0, .panel_id = "dash-bar", .opacity = 0.9});
  if (!expect(layerc::platform::write_profile_bundle(
                  target, preceding_base, std::nullopt, std::nullopt, preceding_companion).ok(),
              "import should start from a committed bundle")) return;
  const auto companion_path = layerc::platform::companion_sidecar_path(target);
  const auto preceding_json = read_file(target);
  const auto preceding_sidecar = read_file(companion_path);
  auto incoming = preceding_base;
  incoming.name = "Imported edit";
  layerc::bundle::ValidatedPack pack;
  layerc::bundle::PackProfile incoming_item{
      .suggested_filename = "a.json",
      .base_json = layerc::core::serialize_profile_json(incoming),
      .profile = incoming};
  incoming_item.module<layerc::core::CompanionModule>() =
      layerc::bundle::ModulePayload<layerc::core::CompanionProfile>{
          .json = layerc::core::serialize_companion_profile_json(
              preceding_companion),
          .profile = preceding_companion};
  pack.profiles.push_back(std::move(incoming_item));
  const std::array choice{layerc::bundle::ImportChoice{
      .profile_index = 0, .action = layerc::bundle::ImportAction::replace}};
  const auto preview = layerc::bundle::preview_profile_import(pack, choice, temp.path());
  if (!expect(!preview.error, "advisory import preview should bind the target")) return;
  const auto transaction = temp.path() / "_transactions" / "interleaved-import";
  auto partial_base = preceding_base;
  partial_base.name = "Interrupted writer";
  auto partial_companion = preceding_companion;
  partial_companion.slots.front().opacity = 0.2;
  std::string journal = "{\"schemaVersion\":2,\"operations\":[";
  const std::array targets{target, companion_path};
  for (std::size_t index{}; index < targets.size(); ++index) {
    if (index != 0) journal.push_back(',');
    journal += "{\"root\":\"config\",\"target\":";
    layerc::core::json::append_quoted(
        journal, layerc::platform::path_to_utf8(
                     targets[index].lexically_relative(temp.path())).value_or(""));
    journal += ",\"existed\":true}";
  }
  journal += "]}\n";
  if (!expect(write_file(transaction / "backup" / "0.bin", preceding_json.value_or("")) &&
                  write_file(transaction / "backup" / "1.bin", preceding_sidecar.value_or("")) &&
                  write_file(transaction / "journal.json", journal) &&
                  write_file(transaction / "published", "2\n") &&
                  write_file(target, layerc::core::serialize_profile_json(partial_base)) &&
                  write_file(companion_path, layerc::core::serialize_companion_profile_json(partial_companion)),
              "interleaving fixture should leave partial primaries and real rollback evidence")) return;
  expect(std::holds_alternative<std::monostate>(
             import_without_reload(pack, preview.choices, temp.path())) &&
             read_file(layerc::platform::recovery_backup_path(target)) == preceding_json &&
             read_file(layerc::platform::recovery_backup_path(companion_path)) == preceding_sidecar,
         "import must rebuild advisory plans after recovery and back up the preceding committed bundle");
}

void malformed_packs_rejected() {
  PackFixture fixture;
  if (!fixture.ready) return;
  const auto &root = fixture.root;
  const auto pack_one = root / "one.layercprofile";
  if (!fixture.export_pack(pack_one)) return;

  auto corrupt = read_file(pack_one).value_or("");
  if (corrupt.size() > 32U)
    corrupt[corrupt.size() / 2U] =
        static_cast<char>(corrupt[corrupt.size() / 2U] ^ 0x5A);
  const auto corrupt_path = root / "corrupt.layercprofile";
  if (expect(write_file(corrupt_path, corrupt), "corrupt ZIP fixture should be written"))
    expect(std::holds_alternative<layerc::bundle::PackError>(
               layerc::bundle::validate_profile_pack(corrupt_path)),
           "corrupt ZIP data or CRC should be rejected");

  const auto traversal = root / "traversal.layercprofile";
  if (expect(write_traversal_zip(traversal), "traversal test ZIP should be created"))
    expect(std::holds_alternative<layerc::bundle::PackError>(
               layerc::bundle::validate_profile_pack(traversal)),
           "parent-directory archive paths should be rejected");

  const auto case_collision = root / "case-collision.layercprofile";
  expect(rewrite_zip(pack_one, case_collision,
                     {.extras = {{"Manifest.json", "{}"}}}) &&
             std::holds_alternative<layerc::bundle::PackError>(
                 layerc::bundle::validate_profile_pack(case_collision)),
         "Windows case-colliding archive entries should be rejected");

  const auto reserved = root / "reserved.layercprofile";
  expect(rewrite_zip(pack_one, reserved, {.extras = {{"CON.json", "{}"}}}) &&
             std::holds_alternative<layerc::bundle::PackError>(
                 layerc::bundle::validate_profile_pack(reserved)),
         "Windows reserved archive components should be rejected");

  const auto undeclared = root / "undeclared.layercprofile";
  expect(rewrite_zip(pack_one, undeclared,
                     {.extras = {{"extra.bin", "not declared"}}}) &&
             std::holds_alternative<layerc::bundle::PackError>(
                 layerc::bundle::validate_profile_pack(undeclared)),
         "undeclared archive entries should be rejected");

  mz_zip_archive manifest_reader{};
  std::string original_manifest;
  if (mz_zip_reader_init_file(&manifest_reader, pack_one.string().c_str(), 0)) {
    size_t size{};
    void *data = mz_zip_reader_extract_file_to_heap(&manifest_reader,
                                                    "manifest.json", &size, 0);
    if (data != nullptr)
      original_manifest.assign(static_cast<const char *>(data), size);
    mz_free(data);
    mz_zip_reader_end(&manifest_reader);
  }
  auto unsupported_manifest = original_manifest;
  const auto schema = unsupported_manifest.find("\"schemaVersion\": 1");
  if (schema != std::string::npos)
    unsupported_manifest.replace(schema, 18, "\"schemaVersion\": 99");
  const auto unsupported = root / "unsupported.layercprofile";
  expect(!original_manifest.empty() && schema != std::string::npos &&
             rewrite_zip(pack_one, unsupported,
                         {.replace_path = "manifest.json",
                          .replacement = unsupported_manifest}) &&
             std::holds_alternative<layerc::bundle::PackError>(
                 layerc::bundle::validate_profile_pack(unsupported)),
         "unsupported manifest schemas should be rejected");

  auto invalid_field_manifest = original_manifest;
  const auto filename_key = invalid_field_manifest.find("\"suggestedFilename\":");
  if (filename_key != std::string::npos) {
    const auto begin = invalid_field_manifest.find('"', filename_key + 20);
    const auto end = invalid_field_manifest.find('"', begin + 1);
    if (begin != std::string::npos && end != std::string::npos)
      invalid_field_manifest.replace(begin, end - begin + 1, "null");
  }
  const auto invalid_field = root / "invalid-field.layercprofile";
  expect(filename_key != std::string::npos &&
             rewrite_zip(pack_one, invalid_field,
                         {.replace_path = "manifest.json",
                          .replacement = invalid_field_manifest}),
         "a malformed nested manifest field should be written");
  const auto invalid_field_result = layerc::bundle::validate_profile_pack(invalid_field);
  const auto *field_error = std::get_if<layerc::bundle::PackError>(&invalid_field_result);
  expect(field_error != nullptr &&
             field_error->message.find("$.profiles[0].suggestedFilename") != std::string::npos,
         "bundle field validation should retain the full descriptor path");

  auto overflow_manifest = original_manifest;
  if (schema != std::string::npos)
    overflow_manifest.replace(schema, 18, "\"schemaVersion\":18446744073709551616");
  const auto overflow_path = root / "overflow-field.layercprofile";
  expect(rewrite_zip(pack_one, overflow_path,
                     {.replace_path = "manifest.json",
                      .replacement = overflow_manifest}),
         "an overflowing manifest integer should be written");
  const auto overflow_result = layerc::bundle::validate_profile_pack(overflow_path);
  const auto *overflow_error = std::get_if<layerc::bundle::PackError>(&overflow_result);
  expect(overflow_error != nullptr &&
             overflow_error->message.find("$.schemaVersion") != std::string::npos,
         "bundle integers beyond uint64 must fail before a narrowing conversion");

  const auto missing_module = root / "missing-module.layercprofile";
  expect(rewrite_zip(pack_one, missing_module,
                     {.skip_path = "modules/000.visual.json"}) &&
             std::holds_alternative<layerc::bundle::PackError>(
                 layerc::bundle::validate_profile_pack(missing_module)),
         "missing declared modules should be rejected");

  ZipRewrite too_many;
  for (int index = 0; index < 256; ++index)
    too_many.extras.emplace_back("extra-" + std::to_string(index) + ".bin", "x");
  const auto excessive_entries = root / "too-many.layercprofile";
  expect(rewrite_zip(pack_one, excessive_entries, too_many) &&
             std::holds_alternative<layerc::bundle::PackError>(
                 layerc::bundle::validate_profile_pack(excessive_entries)),
         "archives beyond the 256-entry limit should be rejected before "
         "extraction");
}

void pack_transaction_recovery() {
  PackFixture fixture;
  if (!fixture.ready) return;
  const auto pack = fixture.root / "one.layercprofile";
  if (!fixture.export_pack(pack)) return;
  auto validated = fixture.validate_pack(pack);
  if (!validated || !fixture.import_both(*validated)) return;
  const auto &imported = fixture.imported;
  expect(std::holds_alternative<std::monostate>(
             layerc::bundle::recover_profile_pack_transactions(imported)),
         "transaction recovery should be idempotent after successful imports");

  const auto recovery_target = imported / "recovery.json";
  const auto uncommitted = imported / "_transactions" / "uncommitted";
  expect(write_file(recovery_target, "new") &&
             write_file(uncommitted / "backup" / "0.bin", "old") &&
             write_file(uncommitted / "journal.json",
                        "{\"schemaVersion\":2,\"operations\":[{\"root\":"
                        "\"config\",\"target\":\"recovery.json\","
                        "\"existed\":true}]}\n") &&
             std::holds_alternative<std::monostate>(
                 layerc::bundle::recover_profile_pack_transactions(imported)) &&
             read_file(recovery_target) == "old",
         "startup recovery should roll back an uncommitted journal");

  const auto committed = imported / "_transactions" / "committed";
  expect(write_file(recovery_target, "published") &&
             write_file(committed / "backup" / "0.bin", "old") &&
             write_file(committed / "journal.json",
                        "{\"schemaVersion\":2,\"operations\":[{\"root\":"
                        "\"config\",\"target\":\"recovery.json\","
                        "\"existed\":true}]}\n") &&
             write_file(committed / "committed", "committed\n") &&
             std::holds_alternative<std::monostate>(
                 layerc::bundle::recover_profile_pack_transactions(imported)) &&
             read_file(recovery_target) == "published",
         "startup recovery should retain a committed publication and clean its "
         "journal");
}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(legacy_visual_schema_validation),
                 LAYERC_TEST_CASE(deterministic_export_and_import),
                 LAYERC_TEST_CASE(copy_import_guards),
                 LAYERC_TEST_CASE(import_preview_conflicts),
                 LAYERC_TEST_CASE(import_planning_after_recovery),
                 LAYERC_TEST_CASE(copy_original_id_reservations),
                 LAYERC_TEST_CASE(copy_choice_order_is_canonical),
                 LAYERC_TEST_CASE(malformed_packs_rejected),
                 LAYERC_TEST_CASE(pack_transaction_recovery))
