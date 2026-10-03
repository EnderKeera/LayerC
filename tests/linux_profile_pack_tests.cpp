#include "layerc/bundle/profile_pack.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store_linux.hpp"
#include "template_png_fixture.hpp"
#include "test_support.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace {

using layerc::test::expect;
using layerc::test::read_file;
using layerc::test::write_file;

layerc::core::GameProfile profile() {
  layerc::core::GameProfile result;
  result.id = "linux-pack";
  result.name = "Linux pack";
  result.game.executable_name = "game";
  result.cooldowns.push_back(
      {.id = "dash", .name = "Dash", .duration = std::chrono::seconds(5)});
  result.panels.push_back({.id = "main",
                           .name = "Main",
                           .bar = {},
                           .cooldown_ids = {"dash"}});
  result.bindings.push_back(
      {.cooldown_id = "dash",
       .chord = {.tokens = {{.device = layerc::core::InputDevice::keyboard,
                             .code = 30}}}});
  return result;
}

layerc::core::VisualProfile visual_profile() {
  layerc::core::VisualProfile result;
  result.profile_id = "linux-pack";
  result.observations.push_back(layerc::core::TemplateState{
      .id = "ready",
      .name = "Ready",
      .search_region = {.x = 0.1, .y = 0.1, .width = 0.5, .height = 0.5},
      .expected_width = 0.2,
      .expected_height = 0.2,
      .references = {{.asset_sha256 =
                          layerc::test::canonical_template_png_fixture().sha256}},
  });
  result.triggers.push_back(
      {.cooldown_id = "dash", .observation_id = "ready"});
  return result;
}

} // namespace

void linux_profile_pack_contracts() {
  const layerc::test::TempDirectory temp("layerc-linux-pack");
  const auto &root = temp.path();
  const layerc::test::ScopedEnvironment home("HOME", root);
  const layerc::test::ScopedEnvironment config_home("XDG_CONFIG_HOME",
                                                  root / "config");
  const layerc::test::ScopedEnvironment data_home("XDG_DATA_HOME",
                                                root / "data");
  if (!expect(home.applied() && config_home.applied() && data_home.applied(),
              "profile-pack test environment should be applied"))
    return;

  const auto roots = layerc::platform::profile_store_roots();
  const auto profile_path = roots.config_profiles / "linux-pack.json";
  const auto second_profile_path =
      roots.config_profiles / "linux-pack-two.json";
  const auto visual = visual_profile();
  auto second_profile = profile();
  second_profile.id = "linux-pack-two";
  second_profile.name = "Linux pack two";
  auto second_visual = visual_profile();
  second_visual.profile_id = second_profile.id;
  const std::string &asset_hash =
      layerc::test::canonical_template_png_fixture().sha256;
  const std::string &asset_png =
      layerc::test::canonical_template_png_fixture().bytes;
  bool passed = expect(layerc::platform::ensure_profile_store_roots(roots),
                       "XDG profile-pack roots should be created");
  passed &= expect(write_file(layerc::platform::profile_visual_asset_path(
                                  profile_path, asset_hash),
                              asset_png) &&
                       write_file(layerc::platform::profile_visual_asset_path(
                                      second_profile_path, asset_hash),
                                  asset_png),
                   "profile-local visual assets should be written");
  passed &= expect(layerc::platform::write_profile_bundle(
                       profile_path, profile(), visual) &&
                       layerc::platform::write_profile_bundle(
                           second_profile_path, second_profile, second_visual),
                   "cross-root profiles and sidecars should be written");

  const auto first_pack = root / "first.layercprofile";
  const auto second_pack = root / "second.layercprofile";
  const std::array selected{profile_path, second_profile_path};
  passed &= expect(std::holds_alternative<std::monostate>(
                       layerc::bundle::export_profile_pack(first_pack,
                                                           selected)) &&
                       std::holds_alternative<std::monostate>(
                           layerc::bundle::export_profile_pack(second_pack,
                                                               selected)),
                   "Linux profile pack should export twice");
  passed &= expect(read_file(first_pack) == read_file(second_pack),
                   "Linux profile pack bytes should be deterministic");

  auto validated_result = layerc::bundle::validate_profile_pack(first_pack);
  auto *validated =
      std::get_if<layerc::bundle::ValidatedPack>(&validated_result);
  passed &= expect(validated != nullptr && validated->profiles.size() == 2,
                   "Linux profile pack should validate");

  std::error_code error;
  std::filesystem::remove_all(roots.config_profiles, error);
  error.clear();
  std::filesystem::remove_all(roots.data_profiles, error);
  passed &= expect(layerc::platform::ensure_profile_store_roots(roots),
                   "import roots should be recreated");
  if (validated != nullptr) {
    const std::array choices{
        layerc::bundle::ImportChoice{
            .profile_index = 0,
            .action = layerc::bundle::ImportAction::replace},
        layerc::bundle::ImportChoice{
            .profile_index = 1,
            .action = layerc::bundle::ImportAction::replace}};
    passed &= expect(std::holds_alternative<std::monostate>(
                         layerc::bundle::import_profile_pack(
                             *validated, choices, roots.config_profiles)),
                     "validated pack should import across XDG roots");
    const auto before = read_file(profile_path);
    std::vector<layerc::bundle::ImportChoice> replace_all;
    for (std::size_t index = 0; index < validated->profiles.size(); ++index)
      replace_all.push_back({.profile_index = index,
                             .action = layerc::bundle::ImportAction::replace});
    const auto preview = layerc::bundle::preview_profile_import(
        *validated, replace_all, roots.config_profiles);
    passed &= expect(!preview.error.has_value(),
                     "an all-existing keep-existing preview is valid");
    auto skipped = preview.choices;
    for (std::size_t index = 0; index < preview.entries.size(); ++index) {
      passed &= expect(preview.entries[index].collides,
                       "existing profiles should be reported as collisions");
      skipped[index].action = layerc::bundle::ImportAction::skip;
    }
    passed &= expect(!layerc::bundle::validate_import_choices(
                          *validated, skipped, roots.config_profiles).has_value(),
                     "the exact all-skipped dry-run plan validates");
    passed &= expect(std::holds_alternative<std::monostate>(
                         layerc::bundle::import_profile_pack(
                             *validated, skipped, roots.config_profiles)),
                     "an all-skipped import succeeds without a transaction");
    passed &= expect(read_file(profile_path) == before &&
                         !std::filesystem::exists(roots.store / "transactions"),
                     "an all-skipped import preserves files without a journal");
    auto invalid = skipped;
    invalid.push_back(invalid.front());
    passed &= expect(std::holds_alternative<layerc::bundle::PackError>(
                         layerc::bundle::import_profile_pack(
                             *validated, invalid, roots.config_profiles)),
                     "an all-skipped plan still rejects duplicate choices");
  }

  const auto loaded = layerc::platform::load_visual_sidecar(profile_path,
                                                             profile());
  const auto second_loaded = layerc::platform::load_visual_sidecar(
      second_profile_path, second_profile);
  passed &= expect(
      std::filesystem::exists(profile_path) &&
          std::filesystem::exists(second_profile_path) &&
          loaded.state == layerc::platform::ModuleSidecarState::valid &&
          second_loaded.state ==
              layerc::platform::ModuleSidecarState::valid &&
          std::filesystem::exists(layerc::platform::profile_visual_asset_path(
              profile_path, asset_hash)) &&
          std::filesystem::exists(layerc::platform::profile_visual_asset_path(
              second_profile_path, asset_hash)),
      "import should restore a shared asset into each profile-local store");

  passed &= expect(layerc::platform::write_profile_bundle(
                       profile_path, profile(), visual).ok(),
                   "bundle save creates primary and recovery sidecars");
  const auto sidecar = layerc::platform::visual_sidecar_path(profile_path);
  auto backup = sidecar;
  backup += ".bak";
  const auto sidecar_bytes = read_file(sidecar);
  const auto backup_bytes = read_file(backup);
  passed &= expect(layerc::platform::write_profile_bundle(
                       profile_path, profile(), std::nullopt) &&
                       layerc::platform::load_visual_sidecar(
                           profile_path, profile()).state ==
                           layerc::platform::ModuleSidecarState::absent,
                   "a removed marker disables the visual sidecar");
  passed &= expect(read_file(sidecar) == sidecar_bytes &&
                       read_file(backup) == backup_bytes,
                   "removal retains bytes instead of running unlocked cleanup");
  passed &= expect(layerc::platform::write_profile_bundle(
                       profile_path, profile(), visual) &&
                       layerc::platform::load_visual_sidecar(
                           profile_path, profile()).state ==
                           layerc::platform::ModuleSidecarState::valid,
                   "a later writer can reactivate retained sidecars");

  (void)passed;
}

int main() {
  return layerc::test::run(
      {{"linux_profile_pack_contracts", linux_profile_pack_contracts}});
}
