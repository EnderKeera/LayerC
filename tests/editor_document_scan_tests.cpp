// Store scan and size-resource cases split from editor_document_store_tests.cpp to keep
// translation units focused.
#include "automation_presets.hpp"
#include "document_commit.hpp"
#include "editor_document_store_fixture.hpp"
#include "profile_document.hpp"
#include "profile_edit.hpp"
#include "test_support.hpp"

#include "layerc/platform/profile_store.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace layerc::core;
using layerc::editor::CommitResult;
using layerc::editor::DocumentChangeKind;
using layerc::editor::ProfileDocument;
using layerc::test::document_store::full_document;
using layerc::test::document_store::IsolatedStore;
using layerc::test::document_store::load;
using layerc::test::document_store::save_new;
using layerc::test::expect;
} // namespace

void profile_scan_summary_preserves_store() {
  const IsolatedStore store;
  auto seeded = full_document();
  auto profile = seeded.profile();
  profile.enabled = false;
  profile.name = "Summary preservation";
  profile.game.executable_name = "summary-game";
  const auto unknown_result =
      json::parse(R"json({"nested":["retained",9007199254740993]})json");
  const auto *unknown = std::get_if<json::Value>(&unknown_result);
  if (!expect(unknown != nullptr, "nested unknown profile field parses")) return;
  profile.extra_fields["futureRoot"] = *unknown;
  const auto profile_id = profile.id;
  if (!expect(!profile.cooldowns.empty() && !profile.panels.empty(),
              "summary fixture includes known cooldown and panel data")) return;
  ProfileDocument document;
  document.start_draft(std::move(profile), layerc::editor::DraftOrigin::fresh,
                       seeded.visual(), seeded.automation());
  const auto saved_path = save_new(store, document);
  if (!saved_path) return;
  const auto disk_before = layerc::test::read_file(*saved_path);
  if (!expect(disk_before.has_value(),
              "normally saved profile bytes can be reread")) return;
  const auto disk_profile_result = parse_profile_json(*disk_before);
  const auto *disk_profile = std::get_if<GameProfile>(&disk_profile_result);
  if (!expect(disk_profile != nullptr,
              "normally saved summary fixture parses completely")) return;

  const auto complete = layerc::platform::scan_profile_directory(store.directory());
  const auto summary =
      layerc::platform::scan_profile_summaries(store.directory());
  const auto *complete_row = complete.find(profile_id);
  const auto *summary_row = summary.find(profile_id);
  if (!expect(complete.error.empty() && complete.faults.empty() &&
                  complete.profiles.size() == 1 && complete_row != nullptr &&
                  complete_row->profile == *disk_profile,
              "default scan retains the complete validated disk profile")) return;
  if (!expect(summary.error.empty() && summary.faults.empty() &&
                  summary.profiles.size() == 1 && summary_row != nullptr,
              "summary scan retains the valid row without scan faults")) return;
  const auto &summary_profile = summary_row->profile;
  expect(complete_row->path == *saved_path && summary_row->path == *saved_path &&
             complete.base_paths == summary.base_paths &&
             complete.base_paths.size() == 1 &&
             complete.base_paths.front() == *saved_path,
         "complete and summary scans reserve the same stored base path");
  static_assert(!std::is_same_v<layerc::platform::ProfileSummary, GameProfile>);
  static_assert(
      !std::is_convertible_v<layerc::platform::ProfileSummary, GameProfile>);
  static_assert(
      !std::is_assignable_v<GameProfile &, layerc::platform::ProfileSummary>);
  expect(summary_profile.id == profile_id &&
             summary_profile.name == "Summary preservation" &&
             !summary_profile.enabled &&
             summary_profile.game.executable_name == "summary-game",
         "summary rows retain only display and identity metadata");
  expect(summary.find(profile_id) == summary_row &&
             summary.path_for(store.directory(), profile_id) == *saved_path,
         "summary scans resolve ids to the stored profile path");
  expect(layerc::editor::profile_list_text(
             summary_profile.name, summary_profile.game.executable_name,
             summary_profile.enabled) ==
             layerc::editor::profile_list_text(*disk_profile),
         "the metadata label overload matches the full-profile label");
  const auto disk_after = layerc::test::read_file(*saved_path);
  expect(disk_after == disk_before,
         "both scan modes leave the original profile bytes unchanged");
  if (!expect(disk_after.has_value(),
              "the unchanged profile bytes remain readable")) return;
  const auto reread_result = parse_profile_json(*disk_after);
  const auto *reread = std::get_if<GameProfile>(&reread_result);
  if (!expect(reread != nullptr,
              "the unchanged disk profile still reloads completely")) return;
  const auto future = reread->extra_fields.find("futureRoot");
  if (!expect(future != reread->extra_fields.end() && future->second.is_object(),
              "full reload retains the unknown root object")) return;
  const auto *nested = future->second.find("nested");
  if (!expect(nested != nullptr && nested->is_array(),
              "full reload retains the unknown nested array")) return;
  const auto &values = nested->as_array();
  expect(values.size() == 2 && values[0].is_string() &&
             values[0].as_string() == "retained" && values[1].is_integer() &&
             values[1].as_integer() == std::int64_t{9'007'199'254'740'993} &&
             disk_before->find("9007199254740993") != std::string::npos,
         "full reload preserves nested strings and the exact large integer");

  const auto neighbor_directory = store.directory() / "summary-neighbors";
  std::error_code error;
  std::filesystem::create_directories(neighbor_directory, error);
  auto neighbor = layerc::editor::make_new_profile();
  neighbor.id = "summary-neighbor";
  neighbor.name = "Valid neighbor";
  neighbor.game.executable_name = "neighbor-game";
  const auto valid_path = neighbor_directory / "valid.json";
  const auto malformed_path = neighbor_directory / "malformed.json";
  const auto oversized_path = neighbor_directory / "oversized.json";
  if (!expect(!error &&
                  layerc::test::write_file(valid_path,
                      serialize_profile_json(neighbor)) &&
                  layerc::test::write_file(malformed_path, "{ invalid") &&
                  layerc::test::write_file(
                      oversized_path,
                      std::string(max_profile_json_bytes + 1, ' ')),
              "valid, malformed and oversized neighbors are written")) return;
  const auto complete_neighbors =
      layerc::platform::scan_profile_directory(neighbor_directory);
  const auto summary_neighbors =
      layerc::platform::scan_profile_summaries(neighbor_directory);
  const auto sorted_paths = [](auto paths) {
    std::sort(paths.begin(), paths.end());
    return paths;
  };
  const auto fault_records = [](const auto &scan) {
    std::vector<std::pair<std::filesystem::path, std::string>> result;
    for (const auto &fault : scan.faults)
      result.emplace_back(fault.path, fault.message);
    std::sort(result.begin(), result.end());
    return result;
  };
  const auto row_identities = [](const auto &scan) {
    std::vector<std::pair<std::string, std::filesystem::path>> result;
    for (const auto &row : scan.profiles)
      result.emplace_back(row.profile.id, row.path);
    std::sort(result.begin(), result.end());
    return result;
  };
  const std::vector<std::filesystem::path> expected_neighbor_paths{
      valid_path, malformed_path, oversized_path};
  const auto has_fault = [](const auto &scan,
                            const std::filesystem::path &path) {
    return std::find_if(scan.faults.begin(), scan.faults.end(),
                        [&](const auto &fault) { return fault.path == path; }) !=
           scan.faults.end();
  };
  expect(complete_neighbors.error.empty() && summary_neighbors.error.empty() &&
             complete_neighbors.faults.size() == 2 &&
             summary_neighbors.faults.size() == 2 &&
             fault_records(complete_neighbors) == fault_records(summary_neighbors) &&
             sorted_paths(complete_neighbors.base_paths) ==
                 sorted_paths(expected_neighbor_paths) &&
             sorted_paths(complete_neighbors.base_paths) ==
                 sorted_paths(summary_neighbors.base_paths) &&
             has_fault(complete_neighbors, malformed_path) &&
             has_fault(complete_neighbors, oversized_path) &&
             row_identities(complete_neighbors) == row_identities(summary_neighbors) &&
             row_identities(complete_neighbors).size() == 1 &&
             row_identities(complete_neighbors).front().first == neighbor.id,
         "both scan modes preserve malformed/oversize faults, reservations and valid identities");

  const auto duplicate_directory = store.directory() / "summary-duplicates";
  error.clear();
  std::filesystem::create_directories(duplicate_directory, error);
  auto first = layerc::editor::make_new_profile();
  first.id = "duplicate-summary-id";
  first.name = "First duplicate";
  first.game.executable_name = "duplicate-game";
  auto second = first;
  second.name = "Second duplicate";
  const auto first_path = duplicate_directory / "first.json";
  const auto second_path = duplicate_directory / "second.json";
  if (!expect(!error &&
                  layerc::test::write_file(first_path,
                      serialize_profile_json(first)) &&
                  layerc::test::write_file(second_path,
                      serialize_profile_json(second)),
              "duplicate-id profiles are written")) return;
  const auto complete_duplicates =
      layerc::platform::scan_profile_directory(duplicate_directory);
  const auto summary_duplicates =
      layerc::platform::scan_profile_summaries(duplicate_directory);
  const std::vector<std::filesystem::path> expected_duplicate_paths{
      first_path, second_path};
  expect(complete_duplicates.error == "duplicate profile id 'duplicate-summary-id'" &&
             summary_duplicates.error == complete_duplicates.error &&
             complete_duplicates.profiles.size() == 1 &&
             summary_duplicates.profiles.size() == 1 &&
             complete_duplicates.profiles.front().profile.id == first.id &&
             summary_duplicates.profiles.front().profile.id == first.id &&
             sorted_paths(complete_duplicates.base_paths) ==
                 sorted_paths(expected_duplicate_paths) &&
             sorted_paths(summary_duplicates.base_paths) ==
                 sorted_paths(expected_duplicate_paths),
         "both scan modes reject duplicate ids and reserve both base paths");
}

void wire_size_resource_contracts() {
  using Snapshot = std::vector<
      std::pair<std::filesystem::path, std::optional<std::string>>>;
  const auto bundle_paths = [](const std::filesystem::path &base) {
    std::vector<std::filesystem::path> paths{
        base, layerc::platform::recovery_backup_path(base)};
    for (const auto kind : {ProfileModuleKind::visual,
                            ProfileModuleKind::automation}) {
      const auto sidecar = layerc::platform::sidecar_paths(base, kind);
      paths.push_back(sidecar.primary);
      paths.push_back(sidecar.backup);
      paths.push_back(sidecar.marker);
    }
    return paths;
  };
  const auto snapshot = [](const std::vector<std::filesystem::path> &paths)
      -> std::optional<Snapshot> {
    Snapshot result;
    result.reserve(paths.size());
    for (const auto &path : paths) {
      std::error_code error;
      const bool exists = std::filesystem::exists(path, error);
      if (error) return std::nullopt;
      if (!exists) {
        result.emplace_back(path, std::nullopt);
        continue;
      }
      auto contents = layerc::test::read_file(path);
      if (!contents) return std::nullopt;
      result.emplace_back(path, std::move(contents));
    }
    return result;
  };

  {
    const IsolatedStore store;
    auto document = full_document();
    const auto path = save_new(store, document);
    if (!path) return;
    const auto sentinel = store.directory() / "uncommitted-sentinel.txt";
    auto paths = bundle_paths(*path);
    paths.push_back(sentinel);
    const auto before = snapshot(paths);
    if (!expect(before.has_value() && !before->back().second,
                "base and sidecar wire-state snapshot includes an absent sentinel")) return;

    // An oversized base is a separate in-memory copy draft: the original saved
    // document stays untouched for the recovery assertions below.
    auto oversized_profile = document.profile();
    oversized_profile.extra_fields["tooLarge"] =
        json::Value(std::string(max_profile_json_bytes, 'x'));
    ProfileDocument oversized;
    oversized.start_draft(std::move(oversized_profile),
                          layerc::editor::DraftOrigin::copy, document.visual(),
                          document.automation());
    const auto add_sentinel = [sentinel](
                                  std::vector<layerc::platform::ProfileFileMutation> &mutations) {
      std::vector<std::byte> bytes{std::byte{0x73}, std::byte{0x65},
                                   std::byte{0x6e}, std::byte{0x74},
                                   std::byte{0x69}, std::byte{0x6e},
                                   std::byte{0x65}, std::byte{0x6c}};
      mutations.push_back(
          {.target = sentinel, .contents = std::move(bytes)});
      return layerc::platform::ProfileWriteResult{};
    };
    const auto rejected = layerc::editor::commit_document(
        oversized, *path, {}, nullptr, {}, add_sentinel);
    const auto after = snapshot(paths);
    expect(rejected.result == CommitResult::write_failed &&
               rejected.write_error ==
                   layerc::platform::ProfileWriteError::resource_limit &&
               layerc::editor::write_failure_text(rejected.write_error).find(
                   "resource limit") != std::string::npos &&
               oversized.profile().extra_fields.contains("tooLarge"),
           "an oversized base save returns a visible resource refusal and retains its draft");
    expect(after.has_value() && *after == *before,
           "oversized base rejection leaves every durable bundle file and sentinel unchanged");

    expect(document.edit_profile([](auto &candidate) { candidate.name = "Recovered"; },
                                 DocumentChangeKind::immediate).accepted(),
           "the recovery rename is one recorded edit");
    const auto recovered =
        layerc::editor::commit_document(document, *path, {}, nullptr);
    const auto reopened = load(*path);
    expect(recovered.ok() && reopened &&
               reopened->profile.name == "Recovered" &&
               !reopened->profile.extra_fields.contains("tooLarge"),
           "a valid base save and reopen recover after resource refusal");
  }

  {
    const IsolatedStore store;
    auto document = full_document();
    const auto path = save_new(store, document);
    if (!path) return;
    const auto valid_automation = *document.automation();
    auto oversized_automation = valid_automation;
    oversized_automation.profile_id = document.profile().id;
    oversized_automation.variables.clear();
    for (std::size_t variable_index = 0; variable_index < 16;
         ++variable_index) {
      AutomationMap value_map;
      for (std::size_t field_index = 0;
           field_index < max_automation_collection_entries; ++field_index) {
        value_map.emplace("f" + std::to_string(field_index),
                          AutomationValue(std::string(
                              max_automation_string_bytes, 'x')));
      }
      oversized_automation.variables.push_back(
          {.id = "bulk" + std::to_string(variable_index),
           .type = AutomationValueType::map,
           .default_value = AutomationValue(std::move(value_map))});
    }
    const auto validation =
        validate_automation_profile(oversized_automation, document.profile());
    if (!expect(validation.message.empty(),
                "oversized automation fixture validates: " + validation.message)) return;
    const auto serialized =
        serialize_automation_profile_json(oversized_automation);
    if (!expect(serialized.size() > max_automation_json_bytes,
                "valid automation fixture exceeds its serialized wire limit")) return;
    auto paths = bundle_paths(*path);
    const auto before = snapshot(paths);
    if (!expect(before.has_value(),
                "automation wire-state snapshot reads every bundle file")) return;

    expect(document.edit_automation(
               [&](std::optional<AutomationProfile> &candidate)
                   -> std::optional<std::string> {
                 candidate = oversized_automation;
                 return std::nullopt;
               },
               DocumentChangeKind::immediate)
                   .accepted(),
           "the oversized graph is field-valid and records one edit");
    const auto rejected =
        layerc::editor::commit_document(document, *path, {}, nullptr);
    const auto after = snapshot(paths);
    expect(rejected.result == CommitResult::write_failed &&
               rejected.write_error ==
                   layerc::platform::ProfileWriteError::resource_limit &&
               document.automation()->variables.size() == 16 &&
               layerc::editor::write_failure_text(rejected.write_error).find(
                   "resource limit") != std::string::npos,
           "an oversized module save returns a resource refusal and retains its draft");
    expect(after.has_value() && *after == *before,
           "oversized module rejection leaves base, sidecars, backups and markers unchanged");

    expect(document.edit_automation(
               [&](std::optional<AutomationProfile> &candidate)
                   -> std::optional<std::string> {
                 candidate = valid_automation;
                 return std::nullopt;
               },
               DocumentChangeKind::immediate)
                   .accepted(),
           "restoring the valid automation graph records one edit");
    const auto recovered =
        layerc::editor::commit_document(document, *path, {}, nullptr);
    const auto reopened = load(*path);
    expect(recovered.ok() && reopened &&
               reopened->modules.get<AutomationModule>().optional_profile() ==
                   std::optional{valid_automation},
           "a valid automation sidecar save and reopen recover after resource refusal");
  }
}
