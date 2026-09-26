#include "core_test_cases.hpp"
#include "layerc/core/update_manifest.hpp"
#include "test_support.hpp"

#include <string>

namespace layerc::test {
namespace {

core::UpdateFile file(std::string path, std::string module, char digit,
                      std::uint64_t size = 10) {
  return {.path = std::move(path),
          .module = std::move(module),
          .size = size,
          .sha256 = std::string(64, digit)};
}

core::UpdateManifest sample_manifest() {
  core::UpdateManifest manifest;
  manifest.channel = "rolling";
  manifest.sequence = 92;
  manifest.commit = std::string(40, 'a');
  manifest.files = {file("README.md", "docs", '1'),
                    file("layerd.exe", "layerd", '2', 2048),
                    file("layerw.exe", "layerw", '3', 1024),
                    file("licenses/miniz/LICENSE", "licenses", '4')};
  return manifest;
}

const core::UpdateManifestError *
parse_error(const std::variant<core::UpdateManifest, core::UpdateManifestError> &parsed) {
  return std::get_if<core::UpdateManifestError>(&parsed);
}

} // namespace

void test_update_manifest_round_trip() {
  const auto manifest = sample_manifest();
  expect(!core::validate_update_manifest(manifest).has_value(),
         "sample update manifest validates");
  const auto json = core::serialize_update_manifest_json(manifest);
  expect(json.starts_with("{\"schemaVersion\":1,\"channel\":\"rolling\","
                          "\"sequence\":92,\"commit\":\""),
         "update manifest serializes fields in a fixed order");
  const auto parsed = core::parse_update_manifest_json(json);
  const auto *round = std::get_if<core::UpdateManifest>(&parsed);
  expect(round != nullptr && *round == manifest,
         "update manifest round-trips through JSON");
  expect(round != nullptr && core::serialize_update_manifest_json(*round) == json,
         "update manifest serialization is byte-stable");
}

void test_update_manifest_rejects_unsafe_input() {
  for (const std::string_view path :
       {"", "../layerd.exe", "a/../b", "/layerd.exe", "a//b", "a\\b",
        ".hidden", "trailing.", "NUL", "con.txt", "Com1.dll", "a b",
        "a/b/c/d/e", "layerd.exe/"}) {
    expect(!core::is_safe_update_path(path),
           "unsafe update path is rejected: " + std::string(path));
  }
  for (const std::string_view path :
       {"layerd.exe", "licenses/miniz/LICENSE", "a/b/c/d", "console.exe",
        "nul_device.txt"}) {
    expect(core::is_safe_update_path(path),
           "safe update path is accepted: " + std::string(path));
  }
  expect(!core::is_safe_update_path(std::string(core::max_update_path_bytes + 1, 'a')),
         "overlong update path is rejected");

  auto unsorted = sample_manifest();
  std::swap(unsorted.files[1], unsorted.files[2]);
  expect(core::validate_update_manifest(unsorted).has_value(),
         "unsorted file list is rejected");

  auto duplicate = sample_manifest();
  duplicate.files[2].path = duplicate.files[1].path;
  expect(core::validate_update_manifest(duplicate).has_value(),
         "duplicate path is rejected");

  auto case_collision = sample_manifest();
  case_collision.files[0].path = "LAYERD.exe";
  case_collision.files.insert(case_collision.files.begin(),
                              file("LAYERD.EXE", "layerd", '5'));
  const auto collision = core::validate_update_manifest(case_collision);
  expect(collision.has_value() && collision->path == "$.files",
         "paths differing only in case are rejected");

  auto bad_hash = sample_manifest();
  bad_hash.files[0].sha256 = std::string(64, 'A');
  expect(core::validate_update_manifest(bad_hash).has_value(),
         "uppercase sha256 is rejected");

  auto bad_commit = sample_manifest();
  bad_commit.commit = "local";
  expect(core::validate_update_manifest(bad_commit).has_value(),
         "short commit is rejected");

  auto zero_sequence = sample_manifest();
  zero_sequence.sequence = 0;
  expect(core::validate_update_manifest(zero_sequence).has_value(),
         "zero sequence is rejected");

  auto oversized = sample_manifest();
  oversized.files[1].size = core::max_update_file_bytes + 1;
  expect(core::validate_update_manifest(oversized).has_value(),
         "oversized file is rejected");

  const auto extra = core::parse_update_manifest_json(
      "{\"schemaVersion\":1,\"channel\":\"rolling\",\"sequence\":1,"
      "\"commit\":\"" + std::string(40, 'b') + "\",\"files\":[],\"extra\":1}");
  expect(parse_error(extra) != nullptr, "unknown top-level field is rejected");

  const auto future = core::parse_update_manifest_json(
      "{\"schemaVersion\":2,\"channel\":\"rolling\",\"sequence\":1,"
      "\"commit\":\"" + std::string(40, 'b') + "\",\"files\":[]}");
  const auto *future_error = parse_error(future);
  expect(future_error != nullptr && future_error->path == "$.schemaVersion",
         "future schema version is rejected at schemaVersion");

  auto tampered = core::serialize_update_manifest_json(sample_manifest());
  tampered.replace(tampered.find("layerw.exe"), 10, "../w.exe");
  const auto *tampered_error = parse_error(core::parse_update_manifest_json(tampered));
  expect(tampered_error != nullptr && tampered_error->path == "$.files[2].path",
         "a traversal path in parsed JSON names the failing entry");

  expect(parse_error(core::parse_update_manifest_json("[]")) != nullptr,
         "non-object manifest root is rejected");
}

void test_update_manifest_plan() {
  const auto target = sample_manifest();

  const auto fresh = core::plan_update(nullptr, target);
  expect(fresh.download.size() == target.files.size() && fresh.reuse.empty() &&
             fresh.remove.empty() && fresh.download_bytes == 10 + 2048 + 1024 + 10,
         "fresh install downloads every file");

  const auto same = core::plan_update(&target, target);
  expect(!same.changes_files() && same.reuse.size() == target.files.size(),
         "identical build reuses every file");

  auto installed = sample_manifest();
  installed.sequence = 91;
  installed.files[1].sha256 = std::string(64, '9');      // layerd changed
  installed.files.erase(installed.files.begin() + 2);    // layerw is new
  installed.files.push_back(file("zz-old.dll", "runtime", '7')); // dropped
  const auto delta = core::plan_update(&installed, target);
  expect(delta.download == std::vector<std::size_t>{1, 2},
         "changed and new files are downloaded");
  expect(delta.reuse == std::vector<std::size_t>{0, 3},
         "unchanged files are reused");
  expect(delta.remove == std::vector<std::size_t>{3},
         "files missing from the target are removed");
  expect(delta.download_bytes == 2048 + 1024, "download size counts only changes");

  auto resized = sample_manifest();
  resized.files[0].size = 11;
  expect(core::plan_update(&resized, target).download ==
             std::vector<std::size_t>{0},
         "a size change alone forces a download");

  expect(core::compare_update_sequence(92, 91) == core::UpdateOrder::older &&
             core::compare_update_sequence(92, 92) == core::UpdateOrder::same &&
             core::compare_update_sequence(92, 93) == core::UpdateOrder::newer,
         "sequence comparison orders builds");
}

} // namespace layerc::test
