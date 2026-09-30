#include "layerc/core/hash.hpp"
#include "layerc/launcher/signature.hpp"
#include "layerc/launcher/update_engine.hpp"
#include "launcher_signature_vectors.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <algorithm>
#include <map>
#include <span>
#include <string>

using namespace layerc;
using layerc::test::expect;
namespace fs = std::filesystem;

namespace {

std::string sha(const std::string_view bytes) {
  return core::sha256(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

// An in-memory release: named assets plus a per-asset fetch counter.
class FakeRelease final : public launcher::ReleaseSource {
public:
  std::map<std::string, std::string, std::less<>> assets;
  std::map<std::string, int, std::less<>> fetches;

  std::optional<std::string> fetch(const std::string_view name,
                                   const std::size_t max_bytes) override {
    ++fetches[std::string(name)];
    const auto found = assets.find(name);
    if (found == assets.end() || found->second.size() > max_bytes)
      return std::nullopt;
    return found->second;
  }

  // Publishes files (path -> contents) as one build and signs it with `key`.
  void publish(const std::uint64_t sequence, const char commit_digit,
               const std::map<std::string, std::string> &files,
               const std::string &channel = "rolling") {
    core::UpdateManifest manifest;
    manifest.channel = channel;
    manifest.sequence = sequence;
    manifest.commit = std::string(40, commit_digit);
    for (const auto &[path, contents] : files) {
      manifest.files.push_back({.path = path,
                                .module = "app",
                                .size = contents.size(),
                                .sha256 = sha(contents)});
      assets[sha(contents)] = contents;
    }
    assets[std::string(launcher::manifest_asset_name)] =
        core::serialize_update_manifest_json(manifest);
    assets[std::string(launcher::signature_asset_name)] =
        "sig:" + sha(assets[std::string(launcher::manifest_asset_name)]);
  }

  int downloads(const std::string_view contents) const {
    const auto found = fetches.find(sha(contents));
    return found == fetches.end() ? 0 : found->second;
  }
};

// Accepts exactly the signature FakeRelease::publish writes.
class FakeVerifier final : public launcher::SignatureVerifier {
public:
  bool verify(const std::string_view manifest,
              const std::string_view signature) override {
    return signature == "sig:" + sha(manifest);
  }
};

struct Fixture {
  test::TempDirectory directory{"layerc-launcher"};
  launcher::InstallLayout layout{directory.path()};
  FakeRelease release;
  FakeVerifier verifier;

  std::variant<launcher::UpdateReport, launcher::UpdateError> update() {
    return launcher::apply_update(layout, "rolling", release, verifier);
  }
  std::optional<std::string> active(const std::string_view path) const {
    const auto file = launcher::active_file(layout, path);
    return file ? test::read_file(*file) : std::nullopt;
  }
};

const launcher::UpdateReport *
report(const std::variant<launcher::UpdateReport, launcher::UpdateError> &result) {
  return std::get_if<launcher::UpdateReport>(&result);
}

const launcher::UpdateError *
error(const std::variant<launcher::UpdateReport, launcher::UpdateError> &result) {
  return std::get_if<launcher::UpdateError>(&result);
}

const std::map<std::string, std::string> build_one{
    {"layerd.exe", "daemon v1"},
    {"layere.exe", "editor v1"},
    {"licenses/miniz/LICENSE", "license"}};

void fresh_install_and_up_to_date() {
  Fixture fixture;
  fixture.release.publish(10, 'a', build_one);

  const auto first = fixture.update();
  const auto *installed = report(first);
  expect(installed && installed->result == launcher::UpdateResult::installed &&
             installed->build_id == "10-aaaaaaaaaaaa" &&
             installed->downloaded_files == 3 && installed->reused_files == 0,
         "a fresh install downloads every file");
  expect(fixture.active("layerd.exe") == "daemon v1" &&
             fixture.active("licenses/miniz/LICENSE") == "license",
         "installed files are reachable through the active build");
  expect(!fs::exists(fixture.layout.staging_directory()),
         "staging is removed after an install");

  const auto second = fixture.update();
  const auto *same = report(second);
  expect(same && same->result == launcher::UpdateResult::up_to_date &&
             same->downloaded_files == 0 &&
             fixture.release.downloads("daemon v1") == 1,
         "the same build is reported up to date without downloading");
}

void delta_update_reuses_unchanged_files() {
  Fixture fixture;
  fixture.release.publish(10, 'a', build_one);
  (void)fixture.update();

  fixture.release.publish(11, 'b',
                          {{"layerd.exe", "daemon v2"},
                           {"layere.exe", "editor v1"},
                           {"layerw.exe", "worker v2"}});
  const auto result = fixture.update();
  const auto *delta = report(result);
  expect(delta && delta->downloaded_files == 2 && delta->reused_files == 1 &&
             delta->downloaded_bytes == 18,
         "only changed and new files are downloaded");
  expect(fixture.release.downloads("editor v1") == 1,
         "an unchanged file is not downloaded again");
  expect(fixture.active("layerd.exe") == "daemon v2" &&
             fixture.active("layere.exe") == "editor v1" &&
             fixture.active("layerw.exe") == "worker v2" &&
             !fixture.active("licenses/miniz/LICENSE"),
         "the new build has exactly the new file set");
  expect(fs::exists(fixture.layout.build_directory("10-aaaaaaaaaaaa")),
         "the previous build is kept for rollback");

  fixture.release.publish(12, 'c', build_one);
  (void)fixture.update();
  expect(!fs::exists(fixture.layout.build_directory("10-aaaaaaaaaaaa")) &&
             fs::exists(fixture.layout.build_directory("11-bbbbbbbbbbbb")),
         "only the active and one previous build are kept");
}

void rejected_releases_leave_the_install_untouched() {
  Fixture fixture;
  fixture.release.publish(10, 'a', build_one);
  (void)fixture.update();
  const auto current = test::read_file(fixture.layout.current_file());

  const auto expect_refused = [&](launcher::UpdateStage stage,
                                  const std::string &message) {
    const auto result = fixture.update();
    const auto *failure = error(result);
    expect(failure && failure->stage == stage, message);
    expect(test::read_file(fixture.layout.current_file()) == current &&
               fixture.active("layerd.exe") == "daemon v1",
           message + " (active build unchanged)");
  };

  fixture.release.publish(11, 'b', {{"layerd.exe", "daemon v2"}});
  fixture.release.assets[std::string(launcher::signature_asset_name)] = "forged";
  expect_refused(launcher::UpdateStage::verify_signature,
                 "a bad signature is refused");

  fixture.release.publish(11, 'b', {{"layerd.exe", "daemon v2"}});
  fixture.release.assets[sha("daemon v2")] = "daemon v2 but tampered";
  expect_refused(launcher::UpdateStage::fetch_file,
                 "a file larger than its manifest size is refused");
  fixture.release.assets[sha("daemon v2")] = "daemon vX";
  expect_refused(launcher::UpdateStage::verify_file,
                 "a file with the wrong hash is refused");
  expect(!fs::exists(fixture.layout.build_directory("11-bbbbbbbbbbbb")),
         "a refused build never reaches app/");

  fixture.release.assets.erase(sha("daemon v2"));
  expect_refused(launcher::UpdateStage::fetch_file,
                 "a missing file asset is refused");

  fixture.release.publish(9, 'c', build_one);
  expect_refused(launcher::UpdateStage::check_sequence,
                 "an older build is refused as a rollback");

  fixture.release.publish(10, 'd', build_one);
  expect_refused(launcher::UpdateStage::check_sequence,
                 "the same sequence for another commit is refused");

  fixture.release.publish(11, 'e', build_one, "beta");
  expect_refused(launcher::UpdateStage::check_channel,
                 "another channel's release is refused");

  fixture.release.assets.clear();
  expect_refused(launcher::UpdateStage::fetch_manifest,
                 "an unreachable release keeps the installed build");
}

void damaged_installs_are_repaired() {
  Fixture fixture;
  fixture.release.publish(10, 'a', build_one);
  (void)fixture.update();

  // A reused file that no longer matches is downloaded again.
  const auto build = fixture.layout.build_directory("10-aaaaaaaaaaaa");
  test::write_file(build / "layere.exe", "corrupted");
  fixture.release.publish(11, 'b',
                          {{"layerd.exe", "daemon v2"}, {"layere.exe", "editor v1"}});
  const auto result = fixture.update();
  const auto *repaired = report(result);
  expect(repaired && repaired->downloaded_files == 2 && repaired->reused_files == 0 &&
             fixture.active("layere.exe") == "editor v1",
         "a damaged local file is downloaded instead of reused");

  // A damaged pointer is reinstalled from scratch rather than blocking launch.
  test::write_file(fixture.layout.current_file(), "../escape\n");
  expect(std::holds_alternative<launcher::UpdateError>(
             launcher::read_installed_build(fixture.layout)),
         "a traversal in the build pointer is reported as damage");
  const auto reinstall = fixture.update();
  const auto *fresh = report(reinstall);
  expect(fresh && fresh->result == launcher::UpdateResult::installed &&
             fresh->reused_files == 0 && fixture.active("layerd.exe") == "daemon v2",
         "a damaged install is repaired with a full download");
  expect(fs::exists(fixture.layout.build_directory("10-aaaaaaaaaaaa")),
         "repair keeps the recorded rollback build");

  // Leftovers from an interrupted update are cleared on the next attempt.
  test::write_file(fixture.layout.staging_directory() / "12-x" / "junk", "junk");
  test::write_file(fixture.layout.build_directory("12-cccccccccccc") / "junk", "junk");
  fixture.release.publish(12, 'c', build_one);
  const auto after_crash = fixture.update();
  expect(report(after_crash) != nullptr && fixture.active("layerd.exe") == "daemon v1" &&
             !fixture.active("junk") && !fs::exists(fixture.layout.staging_directory()),
         "an interrupted update's leftovers are replaced");
}

void rollback_switches_to_the_kept_build() {
  Fixture fixture;
  expect(launcher::rollback(fixture.layout).has_value(),
         "rollback without a previous build fails");

  fixture.release.publish(10, 'a', build_one);
  (void)fixture.update();
  fixture.release.publish(11, 'b', {{"layerd.exe", "daemon v2"}});
  (void)fixture.update();

  expect(!launcher::rollback(fixture.layout).has_value() &&
             fixture.active("layerd.exe") == "daemon v1",
         "rollback activates the previous build");
  expect(!launcher::rollback(fixture.layout).has_value() &&
             fixture.active("layerd.exe") == "daemon v2",
         "a second rollback returns to the newer build");

  expect(!launcher::active_file(fixture.layout, "../layerd.exe") &&
             !launcher::active_file(fixture.layout, "missing.exe"),
         "active_file rejects unsafe and missing paths");
}

void der_signatures_convert_strictly() {
  namespace vectors = test::signature_vectors;
  const auto der = vectors::from_hex(vectors::der_signature_hex);
  const auto raw = launcher::ecdsa_der_to_p1363(der);
  const auto expected = vectors::from_hex(
      "2be7f4422158e0b32e62a30cdee250d70dcf940ce5644d7b181d5dedbdc28004"
      "d467b8d33d16a765d9b0dc7e3476fc8a1c2b8c29648f8eaac289d34acac6cdbb");
  expect(raw && std::equal(raw->begin(), raw->end(), expected.begin(),
                           [](std::uint8_t left, char right) {
                             return left == static_cast<std::uint8_t>(right);
                           }),
         "an openssl DER signature converts to r||s with the sign byte dropped");

  // Short r and s are left-padded to 32 bytes.
  const auto short_values = launcher::ecdsa_der_to_p1363(
      vectors::from_hex("3006020101020102"));
  expect(short_values && (*short_values)[31] == 1 && (*short_values)[63] == 2 &&
             (*short_values)[0] == 0,
         "short integers are left-padded");

  for (const std::string_view bad :
       {"", "3006020101020102ff", "3106020101020102", "3007020101020102",
        "3006020181020102", "300702020001020102", "3006020100020102",
        "30050201010201", "3003020101", "30810602010102010200"}) {
    expect(!launcher::ecdsa_der_to_p1363(vectors::from_hex(bad)),
           "malformed DER is rejected: " + std::string(bad));
  }
  const auto too_wide = "30260221" + std::string("01") + std::string(64, 'f') + "020102";
  expect(!launcher::ecdsa_der_to_p1363(vectors::from_hex(too_wide)),
         "an integer wider than a coordinate is rejected");

  const auto key = launcher::parse_p256_public_key_hex(
      "  " + std::string(vectors::public_key_hex) + "\n");
  expect(key && (*key)[0] == 0x64 && (*key)[63] == 0x57,
         "the uncompressed public key parses without its 04 prefix");
  expect(!launcher::parse_p256_public_key_hex("") &&
             !launcher::parse_p256_public_key_hex(vectors::public_key_hex.substr(2)) &&
             !launcher::parse_p256_public_key_hex(
                 "05" + std::string(vectors::public_key_hex.substr(2))) &&
             !launcher::parse_p256_public_key_hex(
                 std::string(vectors::public_key_hex.substr(0, 128)) + "zz"),
         "missing, compressed-marker, short, or non-hex keys are rejected");
}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(der_signatures_convert_strictly),
                 LAYERC_TEST_CASE(fresh_install_and_up_to_date),
                 LAYERC_TEST_CASE(delta_update_reuses_unchanged_files),
                 LAYERC_TEST_CASE(rejected_releases_leave_the_install_untouched),
                 LAYERC_TEST_CASE(damaged_installs_are_repaired),
                 LAYERC_TEST_CASE(rollback_switches_to_the_kept_build))
