#pragma once

// The one profile-module registry (layer45 F-X1). Every first-party module
// sidecar is one codec struct below plus one entry in ProfileModuleTable.
// Core parsing, `.layercprofile` bundles, the platform sidecar store, and the
// bundle planner fold over the table instead of naming modules.

#include "layerc/core/automation.hpp"
#include "layerc/core/companion.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/types.hpp"
#include "layerc/core/visual.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace layerc::core {

struct ModuleDescriptor {
  ProfileModuleKind kind;
  // Wire name: the base profile's `modules[].kind`, the bundle module kind,
  // and the sidecar file stem (`_<name>/`, `<name>.json`, `.<name>.json`).
  std::string_view name;
  std::size_t max_bytes;
  // Highest module `schemaVersion` this build reads and writes.
  std::uint32_t max_schema;
  // A base profile of at least this schema may declare the module.
  std::uint32_t declarable_from_profile_schema;
  // `.layercprofile` packs require the base profile's declaration and the
  // packed sidecar to agree from this base profile schema on.
  std::uint32_t pack_agreement_from_profile_schema;
};

struct VisualModule {
  using Profile = VisualProfile;
  using Error = VisualError;
  static constexpr ModuleDescriptor descriptor{
      .kind = ProfileModuleKind::visual,
      .name = "visual",
      .max_bytes = max_visual_json_bytes,
      .max_schema = visual_schema_version,
      .declarable_from_profile_schema = 1,
      .pack_agreement_from_profile_schema = 3};
  static auto parse_json(const std::string_view source,
                         const GameProfile &base) {
    return parse_visual_profile_json(source, base);
  }
  static auto parse_value(const json::Value &root, const GameProfile &base) {
    return parse_visual_profile_value(root, base);
  }
  static std::string serialize(const Profile &profile) {
    return serialize_visual_profile_json(profile);
  }
  static bool valid(const Profile &profile, const GameProfile &base) {
    return validate_visual_profile(profile, base).message.empty();
  }
  // Sorted, unique template asset digests the sidecar references.
  static std::vector<std::string> assets(const Profile &profile) {
    std::set<std::string, std::less<>> unique;
    for (const auto &state :
         observations_of<TemplateState>(profile.observations))
      for (const auto &reference : state.references)
        unique.insert(reference.asset_sha256);
    return {unique.begin(), unique.end()};
  }
};

struct AutomationModule {
  using Profile = AutomationProfile;
  using Error = AutomationError;
  static constexpr ModuleDescriptor descriptor{
      .kind = ProfileModuleKind::automation,
      .name = "automation",
      .max_bytes = max_automation_json_bytes,
      .max_schema = 1,
      .declarable_from_profile_schema = 4,
      .pack_agreement_from_profile_schema = 4};
  static auto parse_json(const std::string_view source,
                         const GameProfile &base) {
    return parse_automation_profile_json(source, base);
  }
  static auto parse_value(const json::Value &root, const GameProfile &base) {
    return parse_automation_profile_value(root, base);
  }
  static std::string serialize(const Profile &profile) {
    return serialize_automation_profile_json(profile);
  }
  static bool valid(const Profile &profile, const GameProfile &base) {
    return validate_automation_profile(profile, base).message.empty();
  }
  static std::vector<std::string> assets(const Profile &) { return {}; }
};

struct CompanionModule {
  using Profile = CompanionProfile;
  using Error = CompanionError;
  static constexpr ModuleDescriptor descriptor{
      .kind = ProfileModuleKind::companion,
      .name = "companion",
      .max_bytes = max_companion_sidecar_bytes,
      .max_schema = companion_schema_version,
      .declarable_from_profile_schema = 1,
      .pack_agreement_from_profile_schema = 1};
  static auto parse_json(const std::string_view source,
                         const GameProfile &base) {
    return parse_companion_profile_json(source, base);
  }
  static auto parse_value(const json::Value &root, const GameProfile &base) {
    return parse_companion_profile_value(root, base);
  }
  static std::string serialize(const Profile &profile) {
    return serialize_companion_profile_json(profile);
  }
  static bool valid(const Profile &profile, const GameProfile &base) {
    return !validate_companion_profile(profile, base).has_value();
  }
  static std::vector<std::string> assets(const Profile &) { return {}; }
};

template <typename... Modules> struct ModuleTable {
  static constexpr std::size_t size = sizeof...(Modules);
  static constexpr std::array<ModuleDescriptor, size> descriptors{
      Modules::descriptor...};

  // One value per module, e.g. `each<PackSlot>` is a tuple of PackSlot<M>.
  template <template <typename> typename Slot>
  using each = std::tuple<Slot<Modules>...>;

  template <typename Visitor> static void for_each(Visitor &&visitor) {
    (visitor(Modules{}), ...);
  }

  // Calls `visitor(M{})` for the module of `kind`; every arm must return the
  // same type.
  template <typename Visitor>
  static decltype(auto) visit(const ProfileModuleKind kind,
                              Visitor &&visitor) {
    return visit_from<0>(kind, visitor);
  }

private:
  template <std::size_t Index, typename Visitor>
  static decltype(auto) visit_from(const ProfileModuleKind kind,
                                   Visitor &visitor) {
    using Module = std::tuple_element_t<Index, std::tuple<Modules...>>;
    if constexpr (Index + 1 == size) {
      if (Module::descriptor.kind != kind) std::terminate();
      return visitor(Module{});
    } else {
      if (Module::descriptor.kind == kind) return visitor(Module{});
      return visit_from<Index + 1>(kind, visitor);
    }
  }
};

// THE table. Adding a module adds its codec struct above and one entry here.
using ProfileModuleTable =
    ModuleTable<VisualModule, AutomationModule, CompanionModule>;

[[nodiscard]] constexpr const ModuleDescriptor *
find_module(const ProfileModuleKind kind) noexcept {
  for (const auto &descriptor : ProfileModuleTable::descriptors)
    if (descriptor.kind == kind) return &descriptor;
  return nullptr;
}

[[nodiscard]] constexpr const ModuleDescriptor *
find_module(const std::string_view name) noexcept {
  for (const auto &descriptor : ProfileModuleTable::descriptors)
    if (descriptor.name == name) return &descriptor;
  return nullptr;
}

} // namespace layerc::core
