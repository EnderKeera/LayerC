#pragma once

// Isolated profile-store fixture shared by the editor document store test translation units.
#include "automation_presets.hpp"
#include "document_commit.hpp"
#include "profile_document.hpp"
#include "profile_edit.hpp"
#include "test_support.hpp"

#include "layerc/platform/profile_bundle_load.hpp"
#include "layerc/platform/profile_store.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace layerc::test::document_store {

using namespace layerc::core;

// Isolated store roots for both platform layouts; never the user's store.
class IsolatedStore {
public:
  IsolatedStore()
      : temp_("layerc-editor-store"),
        profiles_("LAYERC_PROFILE_DIR", temp_.path() / "profiles"),
        home_("HOME", temp_.path()),
        config_("XDG_CONFIG_HOME", temp_.path() / "config"),
        data_("XDG_DATA_HOME", temp_.path() / "data") {
    directory_ = layerc::platform::profile_directory();
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    expect(!error && !directory_.empty(), "an isolated profile store exists");
  }
  [[nodiscard]] const std::filesystem::path &directory() const noexcept {
    return directory_;
  }

private:
  layerc::test::TempDirectory temp_;
  layerc::test::ScopedEnvironment profiles_;
  layerc::test::ScopedEnvironment home_;
  layerc::test::ScopedEnvironment config_;
  layerc::test::ScopedEnvironment data_;
  std::filesystem::path directory_;
};

inline layerc::platform::ProfileBundleLoad fixture_load(
    GameProfile profile, std::optional<VisualProfile> visual = {},
    std::optional<AutomationProfile> automation = {}) {
  layerc::platform::ProfileBundleLoad loaded;
  loaded.profile = std::move(profile);
  if (visual)
    loaded.modules.get<VisualModule>() =
        layerc::platform::VisualSidecarLoad::valid(std::move(*visual), {});
  if (automation)
    loaded.modules.get<AutomationModule>() =
        layerc::platform::AutomationSidecarLoad::valid(std::move(*automation), {});
  return loaded;
}

inline layerc::editor::ProfileDocument full_document() {
  auto profile = layerc::editor::make_new_profile();
  profile.game.executable_name = "fixture.exe";
  VisualProfile visual;
  visual.profile_id = profile.id;
  visual.observations.push_back(PixelRule{.id = "ready", .name = "Ready"});
  visual.triggers.push_back({.cooldown_id = "ability", .observation_id = "ready"});
  AutomationProfile automation;
  automation.profile_id = profile.id;
  automation.automations.push_back(layerc::editor::make_automation_preset(
      layerc::editor::AutomationPreset::input_cooldown, "auto", "ability",
      "main"));
  layerc::editor::ProfileDocument document;
  document.start_draft(std::move(profile), layerc::editor::DraftOrigin::fresh,
                       std::move(visual), std::move(automation));
  return document;
}

inline std::optional<layerc::platform::ProfileBundleLoad>
load(const std::filesystem::path &path) {
  auto loaded = layerc::platform::load_profile_bundle(path);
  if (auto *bundle = std::get_if<layerc::platform::ProfileBundleLoad>(&loaded))
    return std::move(*bundle);
  return std::nullopt;
}

inline std::optional<std::filesystem::path> save_new(
    const IsolatedStore &store, layerc::editor::ProfileDocument &document) {
  const auto path = layerc::editor::default_profile_path(store.directory(),
                                                         document.profile());
  const auto outcome =
      layerc::editor::commit_document(document, path, {}, nullptr);
  if (!expect(outcome.ok() && !outcome.write_error,
              "a valid document commits through the durable store"))
    return std::nullopt;
  return path;
}

} // namespace layerc::test::document_store
