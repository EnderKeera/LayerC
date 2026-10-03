// The one profile-module registry (layer45 F-X1): core parsing, bundles, and
// the platform store all read their module facts from this table.

#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/core/profile_modules.hpp"

#include <set>
#include <string>
#include <string_view>

namespace layerc::test {

void test_profile_module_table() {
  using namespace layerc::core;
  std::set<ProfileModuleKind> kinds;
  std::set<std::string_view> names;
  for (const auto &module : ProfileModuleTable::descriptors) {
    kinds.insert(module.kind);
    names.insert(module.name);
    expect(find_module(module.kind) == &module &&
               find_module(module.name) == &module,
           "every descriptor is found by kind and by wire name");
    expect(module.max_bytes > 0 && module.max_schema >= 1 &&
               module.declarable_from_profile_schema >= 1 &&
               module.declarable_from_profile_schema <=
                   profile_schema_version &&
               module.pack_agreement_from_profile_schema <=
                   profile_schema_version,
           "descriptor bounds are positive and within the profile schema");
  }
  expect(kinds.size() == ProfileModuleTable::size &&
             names.size() == ProfileModuleTable::size,
         "module kinds and wire names are unique");
  expect(find_module("unknown") == nullptr, "unknown modules are rejected");

  // Distinct meanings keep distinct fields: visual sidecars may be declared
  // by any base schema, while pack metadata agreement starts at schema 3.
  expect(VisualModule::descriptor.declarable_from_profile_schema == 1 &&
             VisualModule::descriptor.pack_agreement_from_profile_schema == 3 &&
             AutomationModule::descriptor.declarable_from_profile_schema == 4 &&
             VisualModule::descriptor.max_schema == visual_schema_version &&
             CompanionModule::descriptor.max_schema == companion_schema_version,
         "declaration and pack-agreement schemas are separate facts");

  std::size_t visited{};
  ProfileModuleTable::for_each([&]<typename Module>(Module) {
    const auto name = ProfileModuleTable::visit(
        Module::descriptor.kind, []<typename Visited>(Visited) {
          return Visited::descriptor.name;
        });
    expect(name == Module::descriptor.name,
           "visit dispatches every kind to its own codec");
    ++visited;
  });
  expect(visited == ProfileModuleTable::size,
         "for_each visits every table entry once");

  GameProfile base;
  base.id = "game";
  VisualProfile visual;
  visual.profile_id = base.id;
  const auto json = VisualModule::serialize(visual);
  const auto parsed = VisualModule::parse_json(json, base);
  expect(std::holds_alternative<VisualProfile>(parsed) &&
             VisualModule::valid(std::get<VisualProfile>(parsed), base) &&
             VisualModule::assets(visual).empty(),
         "module codecs round-trip through the table entry");
}

} // namespace layerc::test
