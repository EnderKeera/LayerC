#include "layerc/bundle/profile_pack.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"

#include "miniz.h"
#include "test_support.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <variant>
#include <vector>

#include <windows.h>

namespace {

using layerc::test::expect;

layerc::core::GameProfile profile(std::string id, std::string name,
                                  const std::uint16_t key) {
  layerc::core::GameProfile result;
  result.id = std::move(id);
  result.name = std::move(name);
  result.game.executable_name = "game.exe";
  result.cooldowns.push_back(
      {.id = "dash",
       .name = "Dash",
       .duration = std::chrono::seconds(5)});
  result.panels.push_back(
      {.id = "dash-bar",
       .name = "Dash",
       .style = layerc::core::PanelStyle::bar,
       .cooldown_ids = {"dash"}});
  result.bindings.push_back(
      {.cooldown_id = "dash",
       .chord = {.tokens = {{.device = layerc::core::InputDevice::keyboard,
                             .code = key}}}});
  return result;
}

layerc::core::AutomationExpression automation_literal(
    layerc::core::AutomationValue value) {
  layerc::core::AutomationExpression result;
  result.kind = layerc::core::ExpressionKind::literal;
  result.literal = std::move(value);
  return result;
}

layerc::core::AutomationProfile automation_profile(const std::string &id) {
  using namespace layerc::core;
  AutomationProfile result;
  result.profile_id = id;
  result.variables.push_back(
      {.id = "count",
       .type = AutomationValueType::integer,
       .default_value = AutomationValue(std::int64_t{0}),
       .persistent = true});
  AutomationRule rule;
  rule.id = "start";
  rule.event = AutomationEventKind::session_started;
  rule.actions.push_back(
      {.kind = AutomationActionKind::increment_variable,
       .target = "count",
       .value = automation_literal(AutomationValue(std::int64_t{1}))});
  result.automations.push_back(
      {.id = "counter", .name = "Counter", .rules = {rule}});
  return result;
}

std::vector<std::byte> read_all(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  input.seekg(0);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char *>(bytes.data()), size);
  return bytes;
}

std::string read_text(const std::filesystem::path &path) {
  const auto bytes = read_all(path);
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

bool write_all(const std::filesystem::path &path,
               const std::span<const std::byte> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

bool write_text(const std::filesystem::path &path,
                const std::string_view text) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return false;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(output);
}

bool write_traversal_zip(const std::filesystem::path &path) {
  mz_zip_archive zip{};
  if (!mz_zip_writer_init_heap(&zip, 0, 1024)) return false;
  const std::string payload = "{}";
  bool ok = mz_zip_writer_add_mem(&zip, "../manifest.json", payload.data(),
                                  payload.size(), MZ_BEST_COMPRESSION) != 0;
  void *data{};
  size_t size{};
  ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &data, &size);
  mz_zip_writer_end(&zip);
  if (!ok || data == nullptr) return false;
  const auto bytes = std::span(reinterpret_cast<const std::byte *>(data), size);
  const bool written = write_all(path, bytes);
  mz_free(data);
  return written;
}

struct ZipRewrite {
  std::optional<std::string> replace_path;
  std::string replacement;
  std::optional<std::string> skip_path;
  std::vector<std::pair<std::string, std::string>> extras;
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
    if (change.skip_path && path == *change.skip_path) continue;
    if (change.replace_path && path == *change.replace_path) {
      ok = mz_zip_writer_add_mem(&writer, path.c_str(),
                                 change.replacement.data(),
                                 change.replacement.size(),
                                 MZ_BEST_COMPRESSION) != 0;
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
  ok = ok && mz_zip_writer_finalize_heap_archive(
                 &writer, &archive_data, &archive_size) != 0;
  mz_zip_writer_end(&writer);
  mz_zip_reader_end(&reader);
  if (!ok || archive_data == nullptr) {
    mz_free(archive_data);
    return false;
  }
  const bool written = write_all(
      destination,
      std::span(reinterpret_cast<const std::byte *>(archive_data), archive_size));
  mz_free(archive_data);
  return written;
}

} // namespace

int main() {
  bool passed = true;
  const auto root = std::filesystem::temp_directory_path() /
                    ("layerc-pack-tests-" +
                     std::to_string(GetCurrentProcessId()));
  const auto source = root / "source";
  const auto imported = root / "imported";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(source, error);
  std::filesystem::create_directories(imported, error);
  if (error) {
    std::cerr << "FAIL: temporary directories could not be created\n";
    return 1;
  }

  std::wstring previous_profile_dir;
  {
    const DWORD required =
        GetEnvironmentVariableW(L"LAYERC_PROFILE_DIR", nullptr, 0);
    if (required > 0) {
      previous_profile_dir.resize(required - 1U);
      GetEnvironmentVariableW(L"LAYERC_PROFILE_DIR",
                              previous_profile_dir.data(), required);
    }
  }
  SetEnvironmentVariableW(L"LAYERC_PROFILE_DIR", source.c_str());

  const auto first_path = source / "first.json";
  const auto second_path = source / "second.json";
  constexpr std::string_view asset_hash =
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  layerc::core::VisualProfile first_visual;
  first_visual.profile_id = "first";
  first_visual.templates.push_back({
      .id = "ready-template",
      .name = "Ready",
      .search_region = {.x = 0.1, .y = 0.1, .width = 0.4, .height = 0.4},
      .expected_width = 0.2,
      .expected_height = 0.2,
      .references = {{.asset_sha256 = std::string(asset_hash)}},
  });
  first_visual.triggers.push_back(
      {.cooldown_id = "dash", .observation_id = "ready-template"});
  passed &= expect(write_text(source / "_visual" / "assets" /
                                  (std::string(asset_hash) + ".png"),
                              "abc"),
                   "content-addressed test asset should be created");
  const auto first_automation = automation_profile("first");
  passed &= expect(layerc::platform::write_profile_bundle(
                       first_path, profile("first", "First", 30), first_visual,
                       first_automation),
                   "first schema-4 profile, visual, and automation modules should save");
  passed &= expect(
      layerc::platform::write_automation_state(
          first_path, first_automation,
          {{"count", layerc::core::AutomationValue(std::int64_t{9})}}),
      "machine-local automation state should exist beside the export fixture");
  passed &= expect(layerc::platform::write_profile_bundle(
                       second_path, profile("second", "Second", 31),
                       std::nullopt),
                   "second schema-3 profile should save");

  const auto legacy_path = source / "legacy.json";
  auto legacy_base = profile("legacy", "Legacy", 32);
  legacy_base.schema_version = 3;
  legacy_base.modules.push_back(
      {.kind = layerc::core::ProfileModuleKind::visual,
       .schema_version = 1});
  constexpr std::string_view legacy_visual = R"json({
    "schemaVersion":1,"profileId":"legacy","sampleRateHz":10,
    "quality":"balanced","rules":[{
      "id":"ready","name":"Ready","region":{"x":0,"y":0,"width":1,"height":1},
      "predicate":{"type":"brightness","comparison":"above","threshold":100},
      "stableSamples":2,"repeat":"edge"}],
    "triggers":[{"cooldownId":"dash","ruleId":"ready","enabled":true,
      "mode":"pixelOnly","keepInput":false,"onUnavailable":"ignore"}]
  })json";
  passed &= expect(
      write_text(legacy_path,
                 layerc::core::serialize_profile_json(legacy_base)) &&
          write_text(layerc::platform::visual_sidecar_path(legacy_path),
                     legacy_visual),
      "matching schema-1 base metadata and visual sidecar should be staged");
  const std::array legacy_selection{legacy_path};
  const auto legacy_pack = root / "legacy.layercprofile";
  passed &= expect(
      std::holds_alternative<std::monostate>(
          layerc::bundle::export_profile_pack(legacy_pack,
                                              legacy_selection)) &&
          std::holds_alternative<layerc::bundle::ValidatedPack>(
              layerc::bundle::validate_profile_pack(legacy_pack)),
      "matching schema-1 module metadata should validate against the packed source schema before in-memory migration");

  legacy_base.modules.front().schema_version = 2;
  passed &= expect(
      write_text(legacy_path,
                 layerc::core::serialize_profile_json(legacy_base)),
      "mismatched visual module metadata fixture should be staged");
  const auto mismatched_schema_pack = root / "mismatched-schema.layercprofile";
  passed &= expect(
      std::holds_alternative<std::monostate>(
          layerc::bundle::export_profile_pack(mismatched_schema_pack,
                                              legacy_selection)) &&
          std::holds_alternative<layerc::bundle::PackError>(
              layerc::bundle::validate_profile_pack(
                  mismatched_schema_pack)),
      "base module metadata must match the actual packed visual schema");
  std::filesystem::remove(legacy_path, error);
  std::filesystem::remove(layerc::platform::visual_sidecar_path(legacy_path),
                          error);

  const std::array selected{second_path, first_path};
  const auto pack_one = root / "one.layercprofile";
  const auto pack_two = root / "two.layercprofile";
  passed &= expect(
      std::holds_alternative<std::monostate>(
          layerc::bundle::export_profile_pack(pack_one, selected)),
      "multi-profile pack should export");
  passed &= expect(
      std::holds_alternative<std::monostate>(
          layerc::bundle::export_profile_pack(pack_two, selected)),
      "same multi-profile pack should export twice");
  passed &= expect(read_all(pack_one) == read_all(pack_two),
                   "pack export should be byte-for-byte deterministic");
  const auto nested_name =
      layerc::bundle::resolve_suggested_profile_filename(
          "folder/existing.json");
  passed &= expect(nested_name.has_value() &&
                       nested_name->wstring() == L"existing.json",
                   "pack preview and import should resolve nested suggestions to one top-level basename");

  const auto validated_result =
      layerc::bundle::validate_profile_pack(pack_one);
  const auto *validated =
      std::get_if<layerc::bundle::ValidatedPack>(&validated_result);
  passed &= expect(validated != nullptr && validated->profiles.size() == 2,
                   "exported pack should validate with both profiles");
  if (validated != nullptr) {
    const auto first = std::ranges::find(
        validated->profiles, std::string("first"),
        [](const auto &entry) { return entry.profile.id; });
    passed &= expect(
        first != validated->profiles.end() &&
            first->automation_profile.has_value(),
        "automation definitions and defaults should be included in a profile pack");
  }
  if (validated != nullptr) {
    const std::array choices{
        layerc::bundle::ImportChoice{
            .profile_index = 0,
            .action = layerc::bundle::ImportAction::replace},
        layerc::bundle::ImportChoice{
            .profile_index = 1,
            .action = layerc::bundle::ImportAction::replace}};
    passed &= expect(
        std::holds_alternative<std::monostate>(
            layerc::bundle::import_profile_pack(*validated, choices, imported)),
        "validated profiles should import atomically");
    passed &= expect(
        std::filesystem::exists(imported / "first.json") &&
            std::filesystem::exists(imported / "second.json"),
        "suggested filenames should be retained without conflicts");
    passed &= expect(
        std::filesystem::exists(imported / "_visual" / "first.json") &&
            read_all(imported / "_visual" / "assets" /
                     (std::string(asset_hash) + ".png")) ==
                std::vector<std::byte>{std::byte{'a'}, std::byte{'b'},
                                       std::byte{'c'}},
        "sidecars and assets should import beneath the requested destination root");
    passed &= expect(
        std::filesystem::exists(imported / "_automation" / "first.json") &&
            !std::filesystem::exists(
                imported / "_automation_state" /
                "first.json.automation-state.json"),
        "packs should import automation definitions while excluding machine-local state");

    const std::array copy_choice{layerc::bundle::ImportChoice{
        .profile_index = 0, .action = layerc::bundle::ImportAction::copy}};
    passed &= expect(
        std::holds_alternative<std::monostate>(
            layerc::bundle::import_profile_pack(*validated, copy_choice,
                                                imported)),
        "Import Copy should succeed beside existing profiles");
    bool found_copy{};
    for (const auto &entry : std::filesystem::directory_iterator(imported)) {
      if (entry.path().extension() != ".json") continue;
      const auto json = layerc::platform::read_profile_file(entry.path());
      if (!json.has_value()) continue;
      const auto parsed = layerc::core::parse_profile_json(*json);
      const auto *candidate =
          std::get_if<layerc::core::GameProfile>(&parsed);
      found_copy =
          found_copy || (candidate != nullptr &&
                         candidate->id.find("-copy") != std::string::npos);
    }
    passed &= expect(found_copy,
                     "Import Copy should regenerate the profile ID");

    const auto conflict_root = root / "conflicts";
    const auto invalid_target = conflict_root / "existing.JSON";
    passed &= expect(write_text(invalid_target, "{invalid-profile") ,
                     "invalid conflict fixture should be created");
    auto nested_pack = *validated;
    nested_pack.profiles.front().suggested_filename =
        "folder/existing.json";
    const std::array replace_nested{layerc::bundle::ImportChoice{
        .profile_index = 0,
        .action = layerc::bundle::ImportAction::replace}};
    passed &= expect(
        std::holds_alternative<layerc::bundle::PackError>(
            layerc::bundle::import_profile_pack(
                nested_pack, replace_nested, conflict_root)) &&
            read_text(invalid_target) == "{invalid-profile",
        "Replace must reject a case-colliding unreadable JSON target instead of overwriting it without a Copy choice");
  }

  auto corrupt = read_all(pack_one);
  if (corrupt.size() > 32U) corrupt[corrupt.size() / 2U] ^= std::byte{0x5A};
  const auto corrupt_path = root / "corrupt.layercprofile";
  write_all(corrupt_path, corrupt);
  passed &= expect(
      std::holds_alternative<layerc::bundle::PackError>(
          layerc::bundle::validate_profile_pack(corrupt_path)),
      "corrupt ZIP data or CRC should be rejected");

  const auto traversal = root / "traversal.layercprofile";
  passed &= expect(write_traversal_zip(traversal),
                   "traversal test ZIP should be created");
  passed &= expect(
      std::holds_alternative<layerc::bundle::PackError>(
          layerc::bundle::validate_profile_pack(traversal)),
      "parent-directory archive paths should be rejected");

  const auto case_collision = root / "case-collision.layercprofile";
  passed &= expect(
      rewrite_zip(pack_one, case_collision,
                  {.extras = {{"Manifest.json", "{}"}}}) &&
          std::holds_alternative<layerc::bundle::PackError>(
              layerc::bundle::validate_profile_pack(case_collision)),
      "Windows case-colliding archive entries should be rejected");

  const auto reserved = root / "reserved.layercprofile";
  passed &= expect(
      rewrite_zip(pack_one, reserved, {.extras = {{"CON.json", "{}"}}}) &&
          std::holds_alternative<layerc::bundle::PackError>(
              layerc::bundle::validate_profile_pack(reserved)),
      "Windows reserved archive components should be rejected");

  const auto undeclared = root / "undeclared.layercprofile";
  passed &= expect(
      rewrite_zip(pack_one, undeclared,
                  {.extras = {{"extra.bin", "not declared"}}}) &&
          std::holds_alternative<layerc::bundle::PackError>(
              layerc::bundle::validate_profile_pack(undeclared)),
      "undeclared archive entries should be rejected");

  mz_zip_archive manifest_reader{};
  std::string original_manifest;
  if (mz_zip_reader_init_file(&manifest_reader, pack_one.string().c_str(), 0)) {
    size_t size{};
    void *data = mz_zip_reader_extract_file_to_heap(
        &manifest_reader, "manifest.json", &size, 0);
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
  passed &= expect(
      !original_manifest.empty() && schema != std::string::npos &&
          rewrite_zip(pack_one, unsupported,
                      {.replace_path = "manifest.json",
                       .replacement = unsupported_manifest}) &&
          std::holds_alternative<layerc::bundle::PackError>(
              layerc::bundle::validate_profile_pack(unsupported)),
      "unsupported manifest schemas should be rejected");

  const auto missing_module = root / "missing-module.layercprofile";
  passed &= expect(
      rewrite_zip(pack_one, missing_module,
                  {.skip_path = "modules/000.visual.json"}) &&
          std::holds_alternative<layerc::bundle::PackError>(
              layerc::bundle::validate_profile_pack(missing_module)),
      "missing declared modules should be rejected");

  ZipRewrite too_many;
  for (int index = 0; index < 256; ++index)
    too_many.extras.emplace_back("extra-" + std::to_string(index) + ".bin", "x");
  const auto excessive_entries = root / "too-many.layercprofile";
  passed &= expect(
      rewrite_zip(pack_one, excessive_entries, too_many) &&
          std::holds_alternative<layerc::bundle::PackError>(
              layerc::bundle::validate_profile_pack(excessive_entries)),
      "archives beyond the 256-entry limit should be rejected before extraction");

  passed &= expect(
      std::holds_alternative<std::monostate>(
          layerc::bundle::recover_profile_pack_transactions(imported)),
      "transaction recovery should be idempotent after successful imports");

  const auto recovery_target = imported / "recovery.json";
  const auto uncommitted = imported / "_transactions" / "uncommitted";
  passed &= expect(write_text(recovery_target, "new") &&
                       write_text(uncommitted / "backup" / "0.bin", "old") &&
                       write_text(uncommitted / "journal.json",
                                  "{\"schemaVersion\":1,\"operations\":[{\"target\":\"recovery.json\",\"existed\":true}]}\n") &&
                       std::holds_alternative<std::monostate>(
                           layerc::bundle::recover_profile_pack_transactions(
                               imported)) &&
                       read_text(recovery_target) == "old",
                   "startup recovery should roll back an uncommitted journal");

  const auto committed = imported / "_transactions" / "committed";
  passed &= expect(write_text(recovery_target, "published") &&
                       write_text(committed / "backup" / "0.bin", "old") &&
                       write_text(committed / "journal.json",
                                  "{\"schemaVersion\":1,\"operations\":[{\"target\":\"recovery.json\",\"existed\":true}]}\n") &&
                       write_text(committed / "committed", "committed\n") &&
                       std::holds_alternative<std::monostate>(
                           layerc::bundle::recover_profile_pack_transactions(
                               imported)) &&
                       read_text(recovery_target) == "published",
                   "startup recovery should retain a committed publication and clean its journal");

  if (previous_profile_dir.empty())
    SetEnvironmentVariableW(L"LAYERC_PROFILE_DIR", nullptr);
  else
    SetEnvironmentVariableW(L"LAYERC_PROFILE_DIR",
                            previous_profile_dir.c_str());
  std::filesystem::remove_all(root, error);
  return passed ? 0 : 1;
}
