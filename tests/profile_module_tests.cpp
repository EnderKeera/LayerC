// The one profile-module registry (layer45 F-X1): core parsing, bundles, and
// the platform store all read their module facts from this table.

#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/profile.hpp"
#include "layerc/core/profile_modules.hpp"

#include <set>
#include <string>
#include <string_view>
#include <type_traits>

namespace layerc::test {
namespace {

struct FourthModule {
  static constexpr layerc::core::ModuleDescriptor descriptor{
      .kind = static_cast<layerc::core::ProfileModuleKind>(255),
      .name = "fourth-fixture",
      .max_bytes = 1024,
      .max_schema = 1,
      .declarable_from_profile_schema = 1,
      .pack_agreement_from_profile_schema = 1};
};

template <typename Module> struct FixtureSlot {
  int state{};
  std::string diagnostic;
  bool operator==(const FixtureSlot &) const = default;
};

template <typename Module>
using SharedFixtureSlot = FixtureSlot<layerc::core::VisualModule>;

void fourth_module_propagation() {
  using namespace layerc::core;
  // Only this table grows. Storage, loading, iteration, value-copy boundaries,
  // and reconciliation are the exact generic machinery used by the hosts.
  using ExtendedTable =
      ModuleTable<VisualModule, AutomationModule, CompanionModule, FourthModule>;
  using Values = ModuleValues<ExtendedTable, FixtureSlot>;
  std::size_t loaded{};
  auto source = Values::load([&]<typename Module>(Module) {
    ++loaded;
    return FixtureSlot<Module>{1, std::string(Module::descriptor.name)};
  });
  expect(loaded == 4 && source.get<FourthModule>().state == 1 &&
             source.get<FourthModule>().diagnostic == "fourth-fixture",
         "a fourth descriptor automatically participates in loading/storage");
  auto session = source;
  source.get<FourthModule>() = {2, "fourth sidecar fault"};
  std::size_t changed{};
  session.reconcile(source, [&]<typename Module>(Module,
                                               const FixtureSlot<Module> &slot) {
    ++changed;
    expect((std::is_same_v<Module, FourthModule>) && slot.state == 2 &&
               slot.diagnostic == "fourth sidecar fault",
           "fourth-module state and diagnostics propagate to the session");
  });
  expect(changed == 1 && session == source,
         "reconciliation updates every changed slot without a module list");
  session.reconcile(source, [&](auto, const auto &) { ++changed; });
  expect(changed == 1, "unchanged modules do not reconfigure consumers");
  const auto snapshot = session;
  session.get<FourthModule>() = {};
  expect(snapshot.get<FourthModule>().state == 2,
         "module snapshots retain value-copy ownership");
  std::size_t visited{};
  snapshot.for_each([&](auto, const auto &) { ++visited; });
  expect(visited == 4, "const snapshots expose every registered module");

  ModuleValues<ExtendedTable, SharedFixtureSlot> shared_types;
  shared_types.get<FourthModule>().state = 4;
  expect(shared_types.get<VisualModule>().state == 0 &&
             shared_types.get<FourthModule>().state == 4,
         "distinct codecs remain independent even with the same slot type");
}

} // namespace

void test_profile_module_table() {
  fourth_module_propagation();
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
