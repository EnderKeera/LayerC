#pragma once

// Import/save recovery cases shared by the Windows and Linux profile-pack
// suites. Each case receives the platform store's profile directory; the
// suite owns the disposable environment that makes it the active store.

#include "../src/bundle/pack_detail.hpp"
#include "layerc/bundle/profile_pack.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/profile_modules.hpp"
#include "layerc/platform/profile_store.hpp"
#include "template_png_fixture.hpp"
#include "test_support.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace layerc::test::pack_recovery {

inline core::GameProfile base_profile(const std::string &id) {
  core::GameProfile result;
  result.id = id;
  result.name = id;
  result.game.executable_name = "game.exe";
  result.cooldowns.push_back(
      {.id = "dash", .name = "Dash", .duration = std::chrono::seconds(5)});
  result.panels.push_back({.id = "dash-bar",
                           .name = "Dash",
                           .style = core::PanelStyle::bar,
                           .bar = {},
                           .cooldown_ids = {"dash"}});
  result.bindings.push_back(
      {.cooldown_id = "dash",
       .chord = {.tokens = {{.device = core::InputDevice::keyboard,
                             .code = 30}}}});
  return result;
}

// Distinct valid revisions of one module for `base`.
template <typename Module>
typename Module::Profile module_revision(const core::GameProfile &base,
                                         const int revision) {
  using namespace layerc::core;
  if constexpr (std::is_same_v<Module, VisualModule>) {
    VisualProfile result;
    result.profile_id = base.id;
    result.observations.push_back(TemplateState{
        .id = "ready",
        .name = "Ready " + std::to_string(revision),
        .search_region = {.x = 0.1, .y = 0.1, .width = 0.4, .height = 0.4},
        .expected_width = 0.2,
        .expected_height = 0.2,
        .references = {{.asset_sha256 =
                            canonical_template_png_fixture().sha256}},
    });
    result.triggers.push_back(
        {.cooldown_id = "dash", .observation_id = "ready"});
    return result;
  } else if constexpr (std::is_same_v<Module, AutomationModule>) {
    AutomationProfile result;
    result.profile_id = base.id;
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
        {.id = "counter",
         .name = "Counter " + std::to_string(revision),
         .rules = {rule}});
    return result;
  } else {
    static_assert(std::is_same_v<Module, CompanionModule>);
    CompanionProfile result;
    result.profile_id = base.id;
    result.slots.push_back({.slot_index = 0,
                            .panel_id = "dash-bar",
                            .opacity = revision == 1 ? 0.25 : 0.75});
    return result;
  }
}

template <typename Profile>
platform::ModuleIntent<Profile>
intent_for(const std::optional<bundle::ModulePayload<Profile>> &slot) {
  if (slot.has_value()) return slot->profile;
  return platform::RemoveModule{};
}

// The save intents equivalent to importing `item`.
inline platform::ProfileModuleIntents intents_for(const bundle::PackProfile &item) {
  return std::apply(
      [](const auto &...slots) {
        return platform::ProfileModuleIntents{intent_for(slots)...};
      },
      item.modules);
}

template <typename Module>
bundle::PackProfile pack_profile(const core::GameProfile &base,
                                 const typename Module::Profile &module) {
  bundle::PackProfile item{.suggested_filename = base.id + ".json",
                           .base_json = core::serialize_profile_json(base),
                           .profile = base,
                           .modules = {},
                           .asset_sha256 = {}};
  item.module<Module>() = bundle::ModulePayload<typename Module::Profile>{
      .json = Module::serialize(module), .profile = module};
  item.asset_sha256 = Module::assets(module);
  return item;
}

inline bundle::ValidatedPack make_pack(std::vector<bundle::PackProfile> items) {
  bundle::ValidatedPack pack;
  const auto &fixture = canonical_template_png_fixture();
  for (const auto &item : items)
    for (const auto &hash : item.asset_sha256) {
      bundle::PackAsset asset{.sha256 = hash, .bytes = {}};
      for (const char byte : fixture.bytes)
        asset.bytes.push_back(static_cast<std::byte>(byte));
      pack.assets.insert_or_assign(hash, std::move(asset));
    }
  pack.profiles = std::move(items);
  return pack;
}

// Imports through the durable boundary that never broadcasts a reload.
inline bool import_pack(const bundle::ValidatedPack &pack,
                        const std::span<const bundle::ImportChoice> choices,
                        const std::filesystem::path &directory) {
  return std::holds_alternative<bool>(
      bundle::detail::commit_profile_import(pack, choices, directory));
}

enum class Publish { import, save };
enum class Predecessor { usable_backup, unusable, absent };

// Publishes `base` with only `module` through an import or a store save.
template <typename Module>
bool publish(const Publish how, const std::filesystem::path &directory,
             const core::GameProfile &base,
             const typename Module::Profile &module) {
  auto item = pack_profile<Module>(base, module);
  if (how == Publish::save)
    return platform::write_profile_bundle(directory / item.suggested_filename,
                                          base, intents_for(item))
        .ok();
  const std::array choice{bundle::ImportChoice{
      .profile_index = 0, .action = bundle::ImportAction::replace}};
  return import_pack(make_pack({std::move(item)}), choice, directory);
}

// Replacing a sidecar keeps the last usable predecessor (primary or recovered
// backup) as the recovery copy, else falls back to the valid replacement.
template <typename Module>
void sidecar_recovery_case(const std::filesystem::path &directory,
                           const Publish how, const Predecessor predecessor) {
  using platform::ModuleSidecarState;
  const std::string id =
      std::string(Module::descriptor.name) +
      (how == Publish::import ? "-import" : "-save") +
      (predecessor == Predecessor::usable_backup ? "-usable"
       : predecessor == Predecessor::unusable    ? "-unusable"
                                                 : "-absent");
  const auto base = base_profile(id);
  const auto path = directory / (id + ".json");
  const auto paths = platform::sidecar_paths(path, Module::descriptor.kind);
  const auto first = module_revision<Module>(base, 1);
  const auto second = module_revision<Module>(base, 2);
  const auto load = [&] {
    return platform::load_module_sidecar<Module>(path, base);
  };
  constexpr std::string_view corrupt = "{corrupt-sidecar";

  if (predecessor != Predecessor::absent) {
    if (!expect(publish<Module>(Publish::save, directory, base, first),
                id + ": predecessor bundle should save"))
      return;
    bool staged = write_file(paths.primary, corrupt);
    if (predecessor == Predecessor::unusable)
      staged = staged && write_file(paths.backup, corrupt);
    const auto expected = predecessor == Predecessor::usable_backup
                              ? ModuleSidecarState::recovered_backup
                              : ModuleSidecarState::invalid;
    if (!expect(staged && load().state() == expected,
                id + ": predecessor sidecar state should be staged"))
      return;
  }
  if (!expect(publish<Module>(how, directory, base, second),
              id + ": valid replacement should publish"))
    return;
  const auto &recovery =
      predecessor == Predecessor::usable_backup ? first : second;
  expect(read_file(paths.primary) == Module::serialize(second) &&
             read_file(paths.backup) == Module::serialize(recovery) &&
             load().state() == ModuleSidecarState::valid,
         id + ": recovery copy should hold the usable predecessor, else the "
              "replacement");
  const bool damaged = write_file(paths.primary, corrupt);
  const auto recovered = load();
  expect(damaged &&
             recovered.state() == ModuleSidecarState::recovered_backup &&
             recovered.profile() != nullptr &&
             Module::serialize(*recovered.profile()) ==
                 Module::serialize(recovery),
         id + ": a later damaged primary should recover from the backup");
}

// Every module, publication path, and predecessor shape.
inline void sidecar_recovery_cases(const std::filesystem::path &directory) {
  core::ProfileModuleTable::for_each([&]<typename Module>(Module) {
    for (const auto how : {Publish::import, Publish::save})
      for (const auto predecessor :
           {Predecessor::usable_backup, Predecessor::unusable,
            Predecessor::absent})
        sidecar_recovery_case<Module>(directory, how, predecessor);
  });
}

// One rejected profile rejects every profile, sidecar, recovery copy, and
// asset in the batch; the corrected batch publishes all of them together.
inline void batch_import_all_or_nothing(const std::filesystem::path &directory) {
  using core::CompanionModule;
  using core::VisualModule;
  const auto fresh = base_profile("atomic-fresh");
  const auto existing = base_profile("atomic-existing");
  const auto fresh_path = directory / "atomic-fresh.json";
  const auto existing_path = directory / "atomic-existing.json";
  const auto old_companion = module_revision<CompanionModule>(existing, 1);
  const auto new_companion = module_revision<CompanionModule>(existing, 2);
  const auto fresh_sidecar =
      platform::sidecar_paths(fresh_path, VisualModule::descriptor.kind);
  const auto existing_sidecar =
      platform::sidecar_paths(existing_path, CompanionModule::descriptor.kind);
  const auto asset = bundle::detail::asset_path_for_profile(
      fresh_path, canonical_template_png_fixture().sha256);
  if (!expect(publish<CompanionModule>(Publish::save, directory, existing,
                                       old_companion) &&
                  !std::filesystem::exists(fresh_path) &&
                  !std::filesystem::exists(asset),
              "batch fixture should start with one stored profile and no asset"))
    return;
  const std::array watched{existing_path,
                           platform::recovery_backup_path(existing_path),
                           existing_sidecar.primary, existing_sidecar.backup,
                           existing_sidecar.marker};
  std::vector<std::optional<std::string>> before;
  for (const auto &file : watched) before.push_back(read_file(file));

  const auto pack =
      make_pack({pack_profile<VisualModule>(
                     fresh, module_revision<VisualModule>(fresh, 1)),
                 pack_profile<CompanionModule>(existing, new_companion)});
  std::array choices{
      bundle::ImportChoice{.profile_index = 0,
                           .action = bundle::ImportAction::replace},
      bundle::ImportChoice{.profile_index = 1,
                           .action = bundle::ImportAction::replace,
                           .require_absent = true}};
  expect(!import_pack(pack, choices, directory),
         "a profile that fails at commit should reject the whole batch");
  bool unchanged = true;
  for (std::size_t index = 0; index < watched.size(); ++index)
    unchanged = unchanged && read_file(watched[index]) == before[index];
  expect(unchanged && !std::filesystem::exists(fresh_path) &&
             !std::filesystem::exists(fresh_sidecar.primary) &&
             !std::filesystem::exists(fresh_sidecar.backup) &&
             !std::filesystem::exists(asset),
         "a rejected batch should leave no profile, sidecar, backup, or asset");

  choices[1].require_absent = false;
  expect(import_pack(pack, choices, directory) &&
             std::filesystem::exists(fresh_path) &&
             read_file(asset) == canonical_template_png_fixture().bytes &&
             read_file(existing_sidecar.primary) ==
                 CompanionModule::serialize(new_companion) &&
             read_file(existing_sidecar.backup) ==
                 CompanionModule::serialize(old_companion),
         "the corrected batch should publish every profile, asset, and "
         "recovery copy together");
}

} // namespace layerc::test::pack_recovery
