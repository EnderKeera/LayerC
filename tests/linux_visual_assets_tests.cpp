#include "layerc/platform/visual_assets_linux.hpp"
#include "layerc/platform/profile_file_guard.hpp"
#include "layerc/platform/profile_store_linux.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/core/utf8.hpp"
#include "layerc/core/hash.hpp"
#include "layerc/platform/template_png.hpp"
#include "profile_transaction_test.hpp"
#include "template_png_fixture.hpp"
#include "test_support.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace {
namespace platform = layerc::platform;
using layerc::test::expect;
using layerc::test::read_file;
using layerc::test::write_file;

class Fixture {
public:
  Fixture()
      : temp("layerc-visual-assets"), home("HOME", temp.path()),
        config("XDG_CONFIG_HOME", temp.path() / "config"),
        data("XDG_DATA_HOME", temp.path() / "data"),
        override_directory("LAYERC_PROFILE_DIR", std::nullopt) {
    path = platform::profile_directory() / "fixture.json";
    expect(platform::ensure_profile_store_roots(platform::profile_store_roots()),
           "create only the private disposable profile store");
  }
  layerc::test::TempDirectory temp;
  layerc::test::ScopedEnvironment home, config, data, override_directory;
  std::filesystem::path path;
};

void storage_import_and_retention() {
  Fixture fixture;
  const auto image = layerc::test::checker_template_image();
  const auto &canonical = layerc::test::canonical_template_png_fixture();
  auto result = platform::store_profile_template_asset(fixture.path, image);
  const auto *stored = std::get_if<platform::ImportedTemplateAsset>(&result);
  expect(stored && stored->reference.asset_sha256 == canonical.sha256 &&
             stored->image.asset_sha256 == canonical.sha256 &&
             stored->path == platform::profile_visual_asset_path(fixture.path, canonical.sha256),
         "store uses the shared canonical PNG hash and profile-local XDG data path");
  const auto path = stored->path;
  expect(read_file(path) == std::optional<std::string>(canonical.bytes) &&
             !std::filesystem::exists(fixture.path) &&
             !std::filesystem::exists(platform::visual_sidecar_path(fixture.path)),
         "publishing a reference never publishes base or sidecar drafts");
  auto loaded = platform::load_profile_template_asset(fixture.path, canonical.sha256);
  const auto *decoded = std::get_if<layerc::core::TemplateImage>(&loaded);
  expect(decoded && decoded->pixels == image.pixels && decoded->alpha == image.alpha,
         "verified loader reads the just-published canonical pixels");
  const auto modified = std::filesystem::last_write_time(path);
  expect(std::holds_alternative<platform::ImportedTemplateAsset>(
             platform::store_profile_template_asset(fixture.path, image)) &&
             std::filesystem::last_write_time(path) == modified,
         "a validated duplicate is a no-op under writer ownership");
  const auto source = fixture.temp.path() / "import.png";
  expect(write_file(source, canonical.bytes), "write a disposable PNG import");
  expect(std::holds_alternative<platform::ImportedTemplateAsset>(
             platform::import_profile_template_asset(fixture.path, source)),
         "imports use the same canonical encoder and content identity");
  auto masked = image;
  masked.alpha.front() = 0;
  result = platform::store_profile_template_asset(fixture.path, masked);
  const auto *masked_asset = std::get_if<platform::ImportedTemplateAsset>(&result);
  expect(masked_asset && masked_asset->reference.asset_sha256 != canonical.sha256 &&
             read_file(path) == std::optional<std::string>(canonical.bytes),
         "mask edits create a distinct asset without collecting history references");
  const auto copy = platform::store_profile_template_asset(
      fixture.path.parent_path() / "copy.json", image);
  const auto *copied = std::get_if<platform::ImportedTemplateAsset>(&copy);
  expect(copied && copied->path != path && read_file(copied->path) == read_file(path),
         "split-store profiles have independent asset namespaces");
}

void invalid_and_unsafe_destinations() {
  Fixture fixture;
  auto image = layerc::test::checker_template_image();
  for (auto &pixel : image.pixels) pixel = {0, 0, 0};
  expect(std::holds_alternative<platform::VisualAssetError>(
             platform::store_profile_template_asset(fixture.path, image)),
         "flat images cannot become positional templates");
  image = layerc::test::checker_template_image();
  for (const auto &destination : {std::filesystem::path{}, fixture.temp.path() / "other.json",
                                  fixture.path.parent_path() / "bad.txt"})
    expect(std::holds_alternative<platform::VisualAssetError>(
               platform::store_profile_template_asset(destination, image)),
           "invalid and out-of-scope profile paths are refused before writes");
  const auto source = fixture.temp.path() / "bad.png";
  for (const auto &text : {std::string{}, std::string("not a PNG"),
                           std::string(platform::max_template_asset_bytes + 1, 'x')}) {
    expect(write_file(source, text), "prepare a rejected import");
    expect(std::holds_alternative<platform::VisualAssetError>(
               platform::import_profile_template_asset(fixture.path, source)),
           "empty, malformed and over-limit imports are refused");
  }
  const auto &canonical = layerc::test::canonical_template_png_fixture();
  const auto asset = platform::profile_visual_asset_path(fixture.path, canonical.sha256);
  std::filesystem::create_directories(asset.parent_path());
  const auto external = fixture.temp.path() / "outside.png";
  expect(write_file(external, canonical.bytes), "write the outside symlink sentinel");
  std::filesystem::create_symlink(external, asset);
  expect(std::holds_alternative<platform::VisualAssetError>(
             platform::store_profile_template_asset(fixture.path, image)) &&
             read_file(external) == std::optional<std::string>(canonical.bytes),
         "an identical symlink target cannot bypass the writer guard as a no-op");
  std::filesystem::remove(asset);
  std::filesystem::remove(asset.parent_path());
  const auto outside_dir = fixture.temp.path() / "outside-assets";
  std::filesystem::create_directory(outside_dir);
  expect(write_file(outside_dir / asset.filename(), canonical.bytes),
         "write the outside directory sentinel");
  std::filesystem::create_directory_symlink(outside_dir, asset.parent_path());
  expect(std::holds_alternative<platform::VisualAssetError>(
             platform::store_profile_template_asset(fixture.path, image)) &&
             read_file(outside_dir / asset.filename()) == std::optional<std::string>(canonical.bytes),
         "an identical file through a symlink parent is also refused");
}

void complete_batch_refusal_and_retry() {
  Fixture fixture;
  const auto first = layerc::test::checker_template_image();
  auto second = first;
  second.pixels.front() = {0, 255, 0};
  second.asset_sha256.clear();
  const auto first_path = platform::profile_visual_asset_path(
      fixture.path, layerc::test::canonical_template_png_fixture().sha256);
  auto invalid = second;
  invalid.alpha.assign(invalid.alpha.size(), 0);
  const std::array invalid_batch{first, invalid};
  expect(std::holds_alternative<platform::VisualAssetError>(
             platform::store_profile_template_assets(fixture.path, invalid_batch)) &&
             !std::filesystem::exists(first_path),
         "an invalid later reference refuses the complete batch before publication");
  const auto encoded = platform::encode_template_png(second);
  const auto &bytes = std::get<std::vector<std::uint8_t>>(encoded);
  const auto second_path = platform::profile_visual_asset_path(
      fixture.path, layerc::core::sha256(std::as_bytes(std::span(bytes))));
  std::filesystem::create_directories(second_path.parent_path());
  const auto sentinel = fixture.temp.path() / "outside-batch.png";
  expect(write_file(sentinel, "outside"), "write an external batch sentinel");
  std::filesystem::create_symlink(sentinel, second_path);
  const std::array batch{first, second};
  expect(std::holds_alternative<platform::VisualAssetError>(
             platform::store_profile_template_assets(fixture.path, batch)) &&
             !std::filesystem::exists(first_path) &&
             read_file(sentinel) == std::optional<std::string>("outside"),
         "a later unsafe destination cannot leave an earlier template published");
  std::filesystem::remove(second_path);
  const auto result = platform::store_profile_template_assets(fixture.path, batch);
  const auto *stored = std::get_if<std::vector<platform::ImportedTemplateAsset>>(&result);
  expect(stored && stored->size() == 2 && std::filesystem::exists(first_path) &&
             std::filesystem::exists(second_path) &&
             !std::filesystem::exists(fixture.path) &&
             !std::filesystem::exists(platform::visual_sidecar_path(fixture.path)),
         "retry publishes the complete valid asset batch without publishing document drafts");
  auto mismatch = second;
  mismatch.asset_sha256.assign(64, '0');
  const std::array mismatch_batch{first, mismatch};
  const auto original = read_file(first_path);
  expect(std::holds_alternative<platform::VisualAssetError>(
             platform::store_profile_template_assets(fixture.path, mismatch_batch)) &&
             read_file(first_path) == original,
         "a mismatched referenced digest refuses the batch without changing existing assets");
}

void retained_encoded_identity_and_complete_batch() {
  Fixture fixture;
  const auto &canonical = layerc::test::canonical_template_png_fixture();
  // Valid ancillary metadata keeps the same pixels but a distinct encoded identity.
  const std::array<std::uint8_t, 27> metadata{
      0x00U, 0x00U, 0x00U, 0x0fU, 0x74U, 0x45U, 0x58U, 0x74U, 0x6fU,
      0x72U, 0x69U, 0x67U, 0x69U, 0x6eU, 0x00U, 0x70U, 0x6fU, 0x72U,
      0x74U, 0x61U, 0x62U, 0x6cU, 0x65U, 0x39U, 0x6aU, 0x79U, 0x61U};
  auto original = canonical.bytes;
  original.insert(original.end() - 12, metadata.begin(), metadata.end());
  const std::vector<std::uint8_t> original_bytes(original.begin(), original.end());
  const auto normalized = platform::canonical_template_png(original_bytes);
  expect(std::holds_alternative<std::vector<std::uint8_t>>(normalized) &&
             std::get<std::vector<std::uint8_t>>(normalized) == original_bytes,
         "the shared canonical-format contract preserves valid RGBA PNG bytes");
  const auto digest = layerc::core::sha256(std::as_bytes(std::span(original_bytes)));
  const auto path = platform::profile_visual_asset_path(fixture.path, digest);
  std::filesystem::create_directories(path.parent_path());
  expect(digest != canonical.sha256 && write_file(path, original),
         "prepare a portable existing PNG with a distinct valid encoded identity");
  auto loaded = platform::load_profile_template_asset(fixture.path, digest);
  expect(std::holds_alternative<layerc::core::TemplateImage>(loaded),
         "the existing encoded reference is supported by the verified loader");
  const auto retained = std::get<layerc::core::TemplateImage>(loaded);
  const auto modified = std::filesystem::last_write_time(path);
  const std::array retained_batch{retained};
  const auto result = platform::store_profile_template_assets(fixture.path, retained_batch);
  const auto *stored = std::get_if<std::vector<platform::ImportedTemplateAsset>>(&result);
  expect(stored && stored->size() == 1 && stored->front().reference.asset_sha256 == digest &&
             stored->front().image.asset_sha256 == digest &&
             read_file(path) == std::optional<std::string>(original) &&
             std::filesystem::last_write_time(path) == modified,
         "applying an unchanged existing template retains its encoded identity without rewriting");

  auto fresh = layerc::test::checker_template_image();
  fresh.pixels.back() = {51, 71, 81};
  const auto encoded = platform::encode_template_png(fresh);
  const auto &bytes = std::get<std::vector<std::uint8_t>>(encoded);
  const auto fresh_path = platform::profile_visual_asset_path(
      fixture.path, layerc::core::sha256(std::as_bytes(std::span(bytes))));
  auto changed = retained;
  changed.pixels.front() = {17, 91, 201};
  const std::array inconsistent{fresh, retained, changed};
  expect(std::holds_alternative<platform::VisualAssetError>(
             platform::store_profile_template_assets(fixture.path, inconsistent)) &&
             !std::filesystem::exists(fresh_path) &&
             read_file(path) == std::optional<std::string>(original),
         "every retained identity is verified before a new asset can be published");
  const std::array mixed{fresh, retained};
  const auto mixed_result = platform::store_profile_template_assets(fixture.path, mixed);
  const auto *mixed_stored = std::get_if<std::vector<platform::ImportedTemplateAsset>>(&mixed_result);
  expect(mixed_stored && mixed_stored->size() == 2 &&
             mixed_stored->at(1).reference.asset_sha256 == digest &&
             std::filesystem::exists(fresh_path) &&
             read_file(path) == std::optional<std::string>(original) &&
             std::filesystem::last_write_time(path) == modified,
         "a valid mixed batch publishes new content and preserves existing encoded references");
}

void ownership_and_crash_recovery() {
  Fixture fixture;
  const auto image = layerc::test::checker_template_image();
  const auto &canonical = layerc::test::canonical_template_png_fixture();
  const auto asset = platform::profile_visual_asset_path(fixture.path, canonical.sha256);
  {
    platform::TransactionFileLock lock(platform::profile_store_roots());
    expect(lock.locked(), "own the store in a competing writer");
    const auto result = platform::store_profile_template_asset(fixture.path, image);
    const auto *error = std::get_if<platform::VisualAssetError>(&result);
    expect(error && error->message.find("busy") != std::string::npos &&
               !std::filesystem::exists(asset),
           "asset writes fail visibly without touching a busy store");
  }
  expect(std::holds_alternative<platform::ImportedTemplateAsset>(
             platform::store_profile_template_asset(fixture.path, image)),
         "asset publication succeeds after writer ownership is released");
  expect(write_file(asset, "corrupt"), "prepare the corrupt asset repair case");
  expect(std::holds_alternative<platform::ImportedTemplateAsset>(
             platform::store_profile_template_asset(fixture.path, image)) &&
             read_file(asset) == std::optional<std::string>(canonical.bytes),
         "a hash-mismatched plain asset is durably repaired");
  const auto interrupted = layerc::core::text_bytes("interrupted");
  const std::array mutations{platform::ProfileFileMutation{
      .target = asset,
      .contents = std::vector<std::byte>(interrupted.begin(), interrupted.end())}};
  expect(!platform::linux_testing::commit_profile_transaction_fail_after_publish(
             platform::profile_store_roots(), mutations, 1) &&
             read_file(asset) == std::optional<std::string>("interrupted"),
         "simulate crash residue after an asset publication");
  expect(std::holds_alternative<platform::ImportedTemplateAsset>(
             platform::store_profile_template_asset(fixture.path, image)) &&
             read_file(asset) == std::optional<std::string>(canonical.bytes),
         "asset writer recovers the interrupted journal before deduplication");
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(storage_import_and_retention),
                 LAYERC_TEST_CASE(invalid_and_unsafe_destinations),
                 LAYERC_TEST_CASE(ownership_and_crash_recovery),
                 LAYERC_TEST_CASE(complete_batch_refusal_and_retry),
                 LAYERC_TEST_CASE(retained_encoded_identity_and_complete_batch))
