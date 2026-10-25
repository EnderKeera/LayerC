#include "editor_history.hpp"
#include "editor_util.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/visual_assets_windows.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <variant>
#include <vector>

namespace {

using layerc::test::expect;

std::optional<layerc::platform::ImportedTemplateAsset>
publish_fixture_asset(const std::uint8_t salt) {
  layerc::core::TemplateImage image{
      .asset_sha256 = {},
      .width = 8, .height = 8,
      .pixels = std::vector<layerc::core::RgbPixel>(64),
      .alpha = std::vector<std::uint8_t>(64, 255)};
  for (std::size_t index{}; index < image.pixels.size(); ++index)
    image.pixels[index] = {
        static_cast<std::uint8_t>(index * 3U + salt),
        static_cast<std::uint8_t>(255U - index * 2U),
        static_cast<std::uint8_t>(index * 4U)};
  auto published = layerc::platform::store_template_asset(image);
  const auto *asset =
      std::get_if<layerc::platform::ImportedTemplateAsset>(&published);
  if (asset == nullptr) {
    expect(false, "a disposable textured asset must publish");
    return std::nullopt;
  }
  return *asset;
}

layerc::core::VisualProfile visual_fixture(
    const layerc::core::GameProfile &base,
    const layerc::core::TemplateReference &reference) {
  layerc::core::VisualProfile visual;
  visual.profile_id = base.id;
  visual.observations.push_back(layerc::core::TemplateState{.id = "reference", .name = "Reference",
                             .search_region = {0.0, 0.0, 1.0, 1.0},
                             .expected_width = 0.25, .expected_height = 0.25,
                             .references = {reference}});
  visual.triggers.push_back({.cooldown_id = base.cooldowns.front().id,
                             .observation_id = "reference"});
  return visual;
}

void require_available(const layerc::core::VisualProfile &visual) {
  for (const auto &state :
       layerc::core::observations_of<layerc::core::TemplateState>(visual.observations))
    for (const auto &reference : state.references)
      expect(std::holds_alternative<layerc::core::TemplateImage>(
                 layerc::platform::load_template_asset(reference.asset_sha256)),
             "every history reference must still decode with its content hash");
}

} // namespace

void editor_asset_history_contracts() {
  layerc::test::TempDirectory fixture("layerc-history-assets");
  layerc::test::ScopedEnvironment root("LAYERC_PROFILE_DIR", fixture.path());
  if (!expect(root.applied(), "history assets must use a disposable store"))
    return;
  auto base = layerc::editor::make_new_profile();
  base.game.executable_name = "fixture.exe";
  const auto path = fixture.path() / "history.json";
  const auto a = publish_fixture_asset(0);
  const auto b = publish_fixture_asset(1);
  const auto c = publish_fixture_asset(2);
  if (!a || !b || !c) return;
  const std::array visuals{visual_fixture(base, a->reference),
                           visual_fixture(base, b->reference),
                           visual_fixture(base, c->reference)};
  {
    layerc::editor::EditorProfileHistory history;
    std::optional<layerc::core::VisualProfile> current = visuals.front();
    if (!expect(layerc::platform::write_profile_bundle(path, base, current).ok(),
                "the initial assignment must commit as a complete bundle"))
      return;
    history.reset(base, current, std::nullopt);
    for (std::size_t index = 1; index < visuals.size(); ++index) {
      current = visuals[index];
      expect(history.record(base, current, std::nullopt),
             "each A/B/C replacement must retain its history transaction");
      expect(layerc::platform::write_profile_bundle(path, base, current).ok(),
             "successive assignments must commit valid bundles");
      history.rebase(base, current, std::nullopt);
    }
    // Disk now owns C and recovery B. Production save/delete/calibration paths
    // must not invoke the persisted-only maintenance collector (the companion
    // source-contract test enforces that boundary).
    for (const auto &visual : visuals) require_available(visual);
    for (const std::size_t index : {1U, 0U}) {
      const auto previous = history.undo(base, current, std::nullopt);
      if (!expect(previous && previous->visual_profile == visuals[index],
                  "undo must reach B and then A after committing C"))
        return;
      current = previous->visual_profile;
      require_available(*current);
    }
    for (const std::size_t index : {1U, 2U}) {
      const auto next = history.redo(base, current, std::nullopt);
      if (!expect(next && next->visual_profile == visuals[index],
                  "redo must reach B and C with available assets"))
        return;
      current = next->visual_profile;
      require_available(*current);
    }
  }
  // All document/history owners are released before explicit offline cleanup.
  const auto report =
      layerc::platform::garbage_collect_unreferenced_visual_assets();
  expect(report.completed() && report.deleted == 1 && report.retained == 2 &&
             !std::filesystem::exists(a->path) &&
             std::filesystem::exists(b->path) &&
             std::filesystem::exists(c->path),
         "only released A is collectible; recovery B and current C stay live");
}

void editor_asset_pending_publication_contracts() {
  layerc::test::TempDirectory fixture("layerc-unpublished-assets");
  layerc::test::ScopedEnvironment root("LAYERC_PROFILE_DIR", fixture.path());
  if (!expect(root.applied(), "pending assets must use a disposable store"))
    return;
  auto base = layerc::editor::make_new_profile();
  base.game.executable_name = "fixture.exe";
  const auto path = fixture.path() / "unrelated.json";
  {
    // This is calibration-style durable publication, deliberately not yet
    // referenced by a sidecar. No game capture, live IPC or settings are used.
    const auto pending = publish_fixture_asset(3);
    if (!pending) return;
    for (int edit{}; edit < 3; ++edit) {
      base.name += " edited";
      expect(layerc::platform::write_profile_bundle(path, base, std::nullopt).ok(),
             "unrelated profile edits must commit in the same fixture store");
      expect(std::holds_alternative<layerc::core::TemplateImage>(
                 layerc::platform::load_template_asset(
                     pending->reference.asset_sha256)),
             "an unapplied calibration asset must survive unrelated commits");
    }
  }
  const auto report =
      layerc::platform::garbage_collect_unreferenced_visual_assets();
  expect(report.completed() && report.deleted == 1 && report.retained == 0,
         "discarded publication is collectible only after its owner is gone");
}
