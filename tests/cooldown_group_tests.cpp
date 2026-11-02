// Engine/runtime parity for cooldown-group semantics (layer45 F-A11): the
// automation runtime derives its intra-batch cooldown events from the same
// CooldownGroupTopology the engine applies, for every group kind.

#include "core_test_cases.hpp"
#include "automation_test_support.hpp"
#include "test_support.hpp"

#include "layerc/core/automation.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/cooldown_groups.hpp"
#include "layerc/core/live_edit.hpp"

#include <chrono>
#include <set>
#include <string>
#include <vector>

namespace layerc::test {
namespace {
using namespace layerc::core;
using namespace std::chrono_literals;
using IdSet = std::set<std::string>;

std::vector<CooldownDefinition> parity_definitions() {
  std::vector<CooldownDefinition> result;
  for (const char *id : {"a", "b", "c", "d", "e", "f"})
    result.push_back({.id = id, .name = id, .duration = 1s});
  return result;
}

// Overlapping propagation chains, a shared pool, and a propagation group
// that reaches into the pool.
std::vector<CooldownGroup> parity_groups() {
  return {{.id = "chain-ab",
           .name = "AB",
           .mode = CooldownGroupMode::trigger_propagation,
           .cooldown_ids = {"a", "b"}},
          {.id = "chain-bc",
           .name = "BC",
           .mode = CooldownGroupMode::trigger_propagation,
           .cooldown_ids = {"b", "c"}},
          {.id = "pool-de",
           .name = "DE",
           .mode = CooldownGroupMode::shared_pool,
           .cooldown_ids = {"d", "e"},
           .duration = 2s,
           .max_charges = 1},
          {.id = "reach-cd",
           .name = "CD",
           .mode = CooldownGroupMode::trigger_propagation,
           .cooldown_ids = {"c", "d"}}};
}

// The base profile every parity graph is admitted against.
GameProfile parity_base() {
  GameProfile base;
  base.id = "parity";
  base.name = "Parity";
  base.game.executable_name = "parity.exe";
  base.cooldowns = parity_definitions();
  base.cooldown_groups = parity_groups();
  return base;
}

IdSet runtime_events(const AutomationActionKind action, const std::string &target,
                     const AutomationEventKind kind) {
  AutomationProfile profile;
  profile.profile_id = "parity";
  AutomationRule rule;
  rule.id = "act";
  rule.event = AutomationEventKind::input_pressed;
  rule.actions.push_back({.kind = action, .target = target});
  profile.automations.push_back(
      {.id = "parity", .name = "Parity", .enabled = true, .rules = {rule}});
  const auto groups = parity_groups();
  const CooldownEngine engine(parity_definitions(), groups);
  AutomationRuntime runtime(accept_automation(profile, parity_base()), {},
                            engine.group_topology(),
                            [] { return TimePoint{}; });
  expect(runtime.cooldown_groups().data() == engine.group_topology().groups().data(),
         "engine and runtime share definitions rather than deep-copying them");
  const auto result = runtime.process(
      {.kind = AutomationEventKind::input_pressed,
       .timestamp = TimePoint{},
       .source_id = "key"});
  IdSet ids;
  for (const auto &event : result.derived_events)
    if (event.kind == kind) ids.insert(event.source_id);
  return ids;
}

IdSet ready_ids(CooldownEngine &engine) {
  IdSet ids;
  for (const auto &state : engine.snapshot(TimePoint{}))
    if (!state.next_charge_at.has_value()) ids.insert(state.id);
  return ids;
}

// An engine with every owner spent, so a reset is observable as readiness.
CooldownEngine spent_engine() {
  const auto definitions = parity_definitions();
  const auto groups = parity_groups();
  CooldownEngine engine(definitions, groups);
  std::uint64_t origin = 1;
  for (const auto &definition : definitions)
    (void)engine.trigger_with_propagation(definition.id, TimePoint{}, origin++);
  return engine;
}

} // namespace

void test_cooldown_group_engine_runtime_parity() {
  const auto definitions = parity_definitions();
  const auto groups = parity_groups();

  for (const auto &definition : definitions) {
    CooldownEngine engine(definitions, groups);
    const auto triggered =
        engine.trigger_with_propagation(definition.id, TimePoint{}, 1);
    expect(IdSet(triggered.begin(), triggered.end()) ==
               runtime_events(AutomationActionKind::trigger_cooldown,
                              definition.id,
                              AutomationEventKind::cooldown_triggered),
           "trigger_cooldown parity: " + definition.id);

    auto spent = spent_engine();
    expect(ready_ids(spent).empty(), "parity fixture spends every owner");
    expect(spent.reset(definition.id), "reset accepts a defined cooldown");
    expect(ready_ids(spent) ==
               runtime_events(AutomationActionKind::reset_cooldown,
                              definition.id, AutomationEventKind::cooldown_ready),
           "reset_cooldown parity: " + definition.id);
  }

  for (const auto &group : groups) {
    CooldownEngine engine(definitions, groups);
    const auto triggered = engine.trigger_group(group.id, TimePoint{}, 1);
    expect(IdSet(triggered.begin(), triggered.end()) ==
               runtime_events(AutomationActionKind::trigger_group, group.id,
                              AutomationEventKind::cooldown_triggered),
           "trigger_group parity: " + group.id);

    auto spent = spent_engine();
    expect(spent.reset_group(group.id), "reset_group accepts a defined group");
    expect(ready_ids(spent) ==
               runtime_events(AutomationActionKind::reset_group, group.id,
                              AutomationEventKind::cooldown_ready),
           "reset_group parity: " + group.id);
  }

  // Engine staging and reconciliation do not invalidate the runtime's
  // immutable topology. Only explicit runtime reconciliation changes its view.
  CooldownEngine engine(definitions, groups);
  const auto empty_graph = accept_automation({.profile_id = "parity"},
                                             parity_base());
  AutomationRuntime runtime(empty_graph, {}, engine.group_topology(),
                            [] { return TimePoint{}; });
  const auto staged = engine;
  expect(staged.group_topology().groups().data() == runtime.cooldown_groups().data(),
         "staging an engine shares immutable group storage");
  auto changed_groups = groups;
  changed_groups.front().cooldown_ids = {"a", "c"};
  engine.reconfigure(definitions, {}, TimePoint{}, changed_groups);
  expect(std::ranges::equal(runtime.cooldown_groups(), groups),
         "engine reconciliation retains the runtime's old safe topology");
  runtime.reconfigure(empty_graph, engine.group_topology());
  expect(runtime.cooldown_groups().data() == engine.group_topology().groups().data() &&
             std::ranges::equal(runtime.cooldown_groups(), changed_groups),
         "runtime reconciliation adopts the engine's new storage without copying");

  const CooldownGroupTopology topology(groups);
  expect(topology.group_trigger_members("pool-de") ==
             std::vector<std::string>{"d", "e"},
         "a shared pool trigger never propagates");
  expect(topology.group_reset_members("reach-cd") ==
             std::vector<std::string>{"c", "d", "e"},
         "resetting a group restores the whole pool of a pooled member");
}

void test_cooldown_owner_resolution_is_pure() {
  const auto definitions = parity_definitions();
  const CooldownGroupTopology topology(parity_groups());
  const auto pooled = topology.owner(definitions, "e");
  expect(pooled.has_value() && pooled->kind == CooldownOwnerKind::shared_pool &&
             pooled->id == "pool-de" && pooled->duration == 2s,
         "a pooled cooldown resolves to its pool owner");
  const auto standalone = topology.owner(definitions, "a");
  expect(standalone.has_value() &&
             standalone->kind == CooldownOwnerKind::cooldown &&
             standalone->duration == 1s,
         "a standalone cooldown resolves to itself");
  expect(!topology.owner(definitions, "missing").has_value(),
         "an unknown cooldown has no owner");

  // diff_profiles must not throw on definitions a CooldownEngine rejects.
  GameProfile before;
  before.cooldowns = {{.id = "x", .name = "X", .duration = 0ms}};
  GameProfile after = before;
  after.cooldowns.front().duration = 1s;
  bool threw{};
  ProfileDelta delta;
  try {
    delta = diff_profiles(before, after);
  } catch (...) {
    threw = true;
  }
  expect(!threw && delta.reset_cooldown_ids == std::vector<std::string>{"x"},
         "profile diffs resolve owners without constructing engines");
}

} // namespace layerc::test
