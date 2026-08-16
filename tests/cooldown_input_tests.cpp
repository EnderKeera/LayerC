#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/input_matcher.hpp"
#include "layerc/core/hid.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace layerc::test {
void test_cooldown_charges() {
  using namespace std::chrono_literals;
  using namespace layerc::core;

  const std::vector<CooldownDefinition> definitions{CooldownDefinition{
      .id = "dash",
      .name = "Dash",
      .duration = 5s,
      .max_charges = 2,
      .retrigger = RetriggerPolicy::ignore,
      .recovery = RecoveryMode::sequential,
  }};
  CooldownEngine engine(definitions);

  const auto start = TimePoint{};
  expect(engine.trigger("dash", start), "first charge should trigger");
  expect(engine.trigger("dash", start + 1s), "second charge should trigger");
  expect(!engine.trigger("dash", start + 2s),
         "empty cooldown should ignore trigger");

  auto state = engine.snapshot(start + 4s).front();
  expect(state.available_charges == 0,
         "charges should remain empty before first recharge");
  state = engine.snapshot(start + 5s).front();
  expect(state.available_charges == 1,
         "first charge should recover after duration");
  state = engine.snapshot(start + 10s).front();
  expect(state.available_charges == 2 && !state.next_charge_at.has_value(),
         "sequential charges should fully recover");
}

void test_non_stacking_cooldown() {
  using namespace std::chrono_literals;
  using namespace layerc::core;

  const std::vector<CooldownDefinition> definitions{CooldownDefinition{
      .id = "dash",
      .name = "Dash",
      .duration = 5s,
      .max_charges = 2,
      .retrigger = RetriggerPolicy::ignore,
  }};
  CooldownEngine engine(definitions);
  const auto start = TimePoint{};
  expect(engine.trigger("dash", start),
         "non-stacking cooldown should trigger initially");
  expect(!engine.trigger("dash", start + 1s),
         "non-stacking cooldown should ignore early retrigger");

  auto state = engine.snapshot(start + 5s).front();
  expect(state.available_charges == 2 && !state.next_charge_at.has_value(),
         "non-stacking cooldown should be ready after one timer");
  expect(engine.trigger("dash", start + 5s),
         "non-stacking cooldown should trigger after expiry");
  expect(!engine.trigger("dash", start + 6s),
         "non-stacking cooldown should not queue another timer");
}

void test_restart_policy() {
  using namespace std::chrono_literals;
  using namespace layerc::core;

  const std::vector<CooldownDefinition> definitions{CooldownDefinition{
      .id = "blink",
      .name = "Blink",
      .duration = 5s,
      .max_charges = 1,
      .retrigger = RetriggerPolicy::restart,
  }};
  CooldownEngine engine(definitions);
  const auto start = TimePoint{};
  expect(engine.trigger("blink", start),
         "initial restart cooldown trigger should work");
  expect(engine.trigger("blink", start + 4s),
         "restart policy should accept retrigger");
  expect(engine.snapshot(start + 5s).front().available_charges == 0,
         "restart should delay recharge");
  expect(engine.snapshot(start + 9s).front().available_charges == 1,
         "restarted cooldown should eventually recover");
}

void test_restart_with_empty_stacked_charges() {
  using namespace std::chrono_literals;
  using namespace layerc::core;
  const std::vector<CooldownDefinition> definitions{CooldownDefinition{
      .id = "dash",
      .name = "Dash",
      .duration = 5s,
      .max_charges = 2,
      .retrigger = RetriggerPolicy::restart,
      .recovery = RecoveryMode::sequential,
  }};
  CooldownEngine engine(definitions);
  const TimePoint start{};
  expect(engine.trigger("dash", start) &&
             engine.trigger("dash", start + 1s),
         "stacked restart cooldown should consume all charges");
  expect(engine.trigger("dash", start + 2s),
         "restart should accept a trigger with no charges remaining");
  expect(engine.snapshot(start + 5s).front().available_charges == 0,
         "empty-charge restart should replace the pending recharge deadline");
  expect(engine.snapshot(start + 7s).front().available_charges == 1 &&
             engine.snapshot(start + 12s).front().available_charges == 2,
         "restarted stacked charges should recover sequentially");
}

void test_cooldown_control_operations() {
  using namespace std::chrono_literals;
  using namespace layerc::core;
  const std::vector<CooldownDefinition> definitions{{
      .id = "dash", .name = "Dash", .duration = 5s, .max_charges = 2}};
  CooldownEngine engine(definitions);
  const TimePoint start{};
  expect(!engine.reset("missing") &&
             !engine.remaining("missing", start).has_value(),
         "unknown cooldown control operations should report failure");
  expect(engine.trigger("dash", start) &&
             engine.remaining("dash", start + 2s) == 3s,
         "remaining should expose the current monotonic deadline");
  engine.update(start + 5s);
  expect(engine.snapshot(start + 5s).front().available_charges == 2,
         "explicit update should recover a non-stacking charge set");
  expect(engine.trigger("dash", start + 6s) && engine.reset("dash") &&
             engine.remaining("dash", start + 6s) == Milliseconds::zero(),
         "reset should immediately restore a cooldown to ready");

  const std::vector<CooldownDefinition> three_charges{{
      .id = "charges", .name = "Charges", .duration = 5s, .max_charges = 3}};
  CooldownEngine reconfigured(three_charges);
  expect(reconfigured.trigger("charges", start),
         "reconfigure invariant test should consume one charge");
  const std::vector<CooldownDefinition> two_charges{{
      .id = "charges", .name = "Charges", .duration = 5s, .max_charges = 2}};
  reconfigured.reconfigure(two_charges, {}, start + 1s);
  const auto ready = reconfigured.snapshot(start + 1s).front();
  expect(ready.available_charges == ready.max_charges &&
             !ready.next_charge_at.has_value(),
         "shrinking max charges to the available count should clear a stray recharge timer");
}

void test_cooldown_recovery_modes_and_groups() {
  using namespace std::chrono_literals;
  using namespace layerc::core;
  const TimePoint start{};

  const std::vector<CooldownDefinition> parallel_definitions{{
      .id = "charges",
      .name = "Charges",
      .duration = 5s,
      .max_charges = 2,
      .recovery = RecoveryMode::parallel,
  }};
  CooldownEngine parallel(parallel_definitions);
  expect(parallel.trigger("charges", start) &&
             parallel.trigger("charges", start + 1s),
         "parallel recovery should spend independent charges");
  auto state = parallel.snapshot(start + 5s).front();
  expect(state.available_charges == 1 &&
             state.recovery_deadlines.size() == 1,
         "parallel recovery should restore each charge at its own deadline");
  expect(parallel.refund("charges", start + 5s) &&
             parallel.snapshot(start + 5s).front().available_charges == 2,
         "parallel refunds should cancel the newest outstanding deadline");

  const std::vector<CooldownDefinition> sequential_definitions{{
      .id = "charges",
      .name = "Charges",
      .duration = 5s,
      .max_charges = 2,
      .recovery = RecoveryMode::sequential,
  }};
  CooldownEngine sequential(sequential_definitions);
  expect(sequential.trigger("charges", start) &&
             sequential.trigger("charges", start + 1s) &&
             sequential.snapshot(start + 5s).front().available_charges == 1 &&
             sequential.snapshot(start + 10s).front().available_charges == 2,
         "sequential recovery should restore one charge per duration");

  const std::vector<CooldownDefinition> definitions{
      {.id = "dash", .name = "Dash", .duration = 3s},
      {.id = "blink", .name = "Blink", .duration = 4s}};
  const std::vector<CooldownGroup> groups{
      {.id = "movement-pool",
       .name = "Movement Pool",
       .mode = CooldownGroupMode::shared_pool,
       .cooldown_ids = {"dash", "blink"},
       .duration = 5s,
       .max_charges = 2,
       .recovery = RecoveryMode::parallel},
      {.id = "movement-all",
       .name = "Movement All",
       .mode = CooldownGroupMode::trigger_propagation,
       .cooldown_ids = {"dash", "blink"}}};
  CooldownEngine grouped(definitions, groups);
  expect(grouped.trigger("dash", start),
         "a shared-pool member should trigger its pool");
  const auto shared = grouped.snapshot(start);
  expect(shared.size() == 2 && shared[0].available_charges == 1 &&
             shared[1].available_charges == 1,
         "shared-pool members should expose identical charge state");
  expect(grouped.reset_group("movement-pool") &&
             grouped.snapshot(start).front().available_charges == 2,
         "resetting a shared pool should restore its common state once");
  expect(!grouped.trigger_group("movement-all", start, 77).empty() &&
             grouped.snapshot(start).front().available_charges == 1,
         "propagation should trigger every distinct member state without recursion");

  const std::vector<CooldownDefinition> propagated_definitions{
      {.id = "one", .name = "One", .duration = 3s},
      {.id = "two", .name = "Two", .duration = 3s},
      {.id = "three", .name = "Three", .duration = 3s}};
  const std::vector<CooldownGroup> propagated_groups{
      {.id = "first",
       .name = "First",
       .mode = CooldownGroupMode::trigger_propagation,
       .cooldown_ids = {"one", "two"}},
      {.id = "second",
       .name = "Second",
       .mode = CooldownGroupMode::trigger_propagation,
       .cooldown_ids = {"two", "three"}}};
  CooldownEngine propagated(propagated_definitions, propagated_groups);
  const auto triggered =
      propagated.trigger_with_propagation("one", start, 91);
  const auto states = propagated.snapshot(start);
  expect(triggered.size() == 3 &&
             std::ranges::all_of(states, [](const auto &state) {
               return state.available_charges == 0;
             }),
         "a direct cooldown trigger should transitively fan through every "
         "overlapping propagation group exactly once");
}
void test_snapshot_reuse() {
  using namespace std::chrono_literals;
  using namespace layerc::core;

  const std::vector<CooldownDefinition> definitions{CooldownDefinition{
      .id = "dash",
      .name = "Dash",
      .duration = 5s,
  }};
  CooldownEngine engine(definitions);
  std::vector<CooldownState> states;
  states.reserve(8);
  const auto capacity = states.capacity();
  const auto *storage = states.data();
  expect(engine.trigger("dash", TimePoint{}),
         "snapshot reuse test should begin an actual timeline");
  engine.snapshot(TimePoint{} + 1s, states);
  expect(states.capacity() == capacity && states.data() == storage &&
             states.front().next_charge_at.has_value() &&
             states.front().available_charges == 0,
         "cooldown snapshots should reuse reserved caller storage and update it");
  engine.snapshot(TimePoint{} + 5s, states);
  expect(states.data() == storage && states.front().available_charges == 1 &&
             !states.front().next_charge_at.has_value(),
         "reused snapshot storage should receive later state transitions");
}

void test_input_chord_edges() {
  using namespace layerc::core;
  const InputToken shift{InputDevice::keyboard, 16, 0};
  const InputToken e{InputDevice::keyboard, 69, 0};
  const std::vector<InputBinding> bindings{
      InputBinding{.cooldown_id = "dash", .chord = InputChord{{shift, e}}}};
  InputMatcher matcher(bindings);

  expect(matcher.process(InputEvent{shift, true}).empty(),
         "partial chord should not trigger");
  auto triggers = matcher.process(InputEvent{e, true});
  expect(triggers.size() == 1 && triggers.front() == "dash",
         "completed chord should trigger once");
  expect(matcher.process(InputEvent{e, true}).empty(),
         "held chord should not repeat");
  expect(matcher.process(InputEvent{e, false}).empty(),
         "release should only rearm chord");
  expect(matcher.process(InputEvent{e, true}).size() == 1,
         "re-pressed chord should trigger again");
}

void test_input_matcher_state_management() {
  using namespace layerc::core;
  const InputToken shift{InputDevice::keyboard, 0x2A, 0};
  const InputToken key{InputDevice::keyboard, 0x12, 0};
  InputMatcher matcher(std::vector<InputBinding>{{
      .cooldown_id = "dash", .chord = InputChord{{shift, key}}}});

  matcher.seed(shift, true);
  matcher.seed(key, true);
  expect(matcher.process(InputEvent{key, true}).empty(),
         "seeding a held chord should not synthesize a new edge");
  (void)matcher.process(InputEvent{key, false});
  expect(matcher.process(InputEvent{key, true}) ==
             std::vector<std::string>{"dash"},
         "a seeded chord should rearm after a real release");

  matcher.clear();
  expect(matcher.process(InputEvent{shift, true}).empty() &&
             matcher.process(InputEvent{key, true}) ==
                 std::vector<std::string>{"dash"},
         "clear should discard held and active chord state");

  const InputToken mouse{InputDevice::mouse, 4, 0};
  const InputToken controller{InputDevice::controller, 13, 2};
  matcher.replace_bindings(std::vector<InputBinding>{
      {.cooldown_id = "mouse", .chord = InputChord{{mouse}}},
      {.cooldown_id = "controller", .chord = InputChord{{controller}}},
  });
  expect(matcher.process(InputEvent{key, true}).empty() &&
             matcher.process(InputEvent{mouse, true}) ==
                 std::vector<std::string>{"mouse"} &&
             matcher.process(InputEvent{controller, true}) ==
                 std::vector<std::string>{"controller"},
         "replacement bindings should route mouse and indexed controller edges");
}

void test_hid_boolean_evaluation() {
  using namespace layerc::core;
  HidControlBinding axis{.activation = HidActivationMode::axis_positive,
                         .threshold = 0.6,
                         .hysteresis = 0.1};
  HidControlState state;
  HidControlBinding button{.activation = HidActivationMode::button};
  auto edge = evaluate_hid_control(button, {.button_active = false}, state);
  expect(edge == false && state.initialized && !state.pressed,
         "button baselines should publish an initial released state");
  edge = evaluate_hid_control(button, {.button_active = true}, state);
  expect(edge == true && state.pressed,
         "button reports should publish their press edge");
  expect(!evaluate_hid_control(button, {.button_active = true}, state),
         "held HID buttons should not repeat");

  HidControlBinding discrete{.activation = HidActivationMode::equals,
                             .discrete_value = 8};
  state = {};
  edge = evaluate_hid_control(
      discrete, {.value = 8, .logical_min = 0, .logical_max = 8}, state);
  expect(edge == true,
         "hat and array values should activate through discrete equality");
  edge = evaluate_hid_control(
      discrete, {.value = 0, .logical_min = 0, .logical_max = 8}, state);
  expect(edge == false, "discrete HID values should publish release edges");

  edge = evaluate_hid_control(
      axis, {.value = 70, .logical_min = 0, .logical_max = 100}, state);
  expect(edge == true && state.pressed,
         "positive HID axes should cross their normalized threshold");
  edge = evaluate_hid_control(
      axis, {.value = 55, .logical_min = 0, .logical_max = 100}, state);
  expect(!edge.has_value() && state.pressed,
         "HID axis hysteresis should retain the pressed state");
  edge = evaluate_hid_control(
      axis, {.value = 45, .logical_min = 0, .logical_max = 100}, state);
  expect(edge == false && !state.pressed,
         "HID axis should release below threshold minus hysteresis");

  HidControlBinding negative{.activation = HidActivationMode::axis_negative,
                             .threshold = 0.75,
                             .hysteresis = 0.1};
  state = {};
  edge = evaluate_hid_control(
      negative, {.value = -90, .logical_min = -100, .logical_max = 100},
      state);
  expect(edge == true,
         "centered negative HID axes should normalize before thresholding");
  edge = evaluate_hid_control(
      negative, {.value = 0, .logical_min = -100, .logical_max = 100},
      state);
  expect(edge == false,
         "centered negative HID axes should release near center");

  HidControlBinding relative{
      .activation = HidActivationMode::relative_positive};
  state = {};
  edge = evaluate_hid_control(
      relative, {.relative = true, .value = 3, .logical_max = 127}, state);
  expect(edge == true,
         "positive relative HID controls should produce a momentary press");
  edge = evaluate_hid_control(
      relative, {.relative = true, .value = 0, .logical_max = 127}, state);
  expect(edge == false,
         "relative HID controls should rearm at zero");

  edge = evaluate_hid_control(axis, {.available = false}, state);
  expect(!edge.has_value() && !state.initialized,
         "unavailable HID samples should clear their stable baseline");
  expect(is_system_keyboard_or_mouse(
             {.usage_page = 1, .usage = 6}) &&
             !is_system_keyboard_or_mouse(
                 {.usage_page = 1, .usage = 5}),
         "system keyboard and mouse top-level collections should be detected");

  GameProfile profile;
  profile.hid_bindings = {{.id = "first"}, {.id = "orphan"}, {.id = "last"}};
  profile.bindings = {
      {.cooldown_id = "a",
       .chord = InputChord{{{InputDevice::generic_hid, 3, 0}}}},
      {.cooldown_id = "b",
       .chord = InputChord{{{InputDevice::generic_hid, 1, 0}}}},
  };
  compact_hid_bindings(profile);
  expect(profile.hid_bindings.size() == 2 &&
             profile.hid_bindings[0].id == "last" &&
             profile.hid_bindings[1].id == "first" &&
             profile.bindings[0].chord.tokens[0].code == 1 &&
             profile.bindings[1].chord.tokens[0].code == 2,
         "HID descriptor compaction should remove orphans and remap tokens");

  GameProfile malformed;
  malformed.hid_bindings = {{.id = "only"}};
  malformed.bindings = {
      {.cooldown_id = "unsafe-zero",
       .chord = InputChord{{{InputDevice::keyboard, 30, 0},
                            {InputDevice::generic_hid, 0, 0}}}},
      {.cooldown_id = "unsafe-range",
       .chord = InputChord{{{InputDevice::generic_hid, 2, 0}}}},
  };
  compact_hid_bindings(malformed);
  expect(malformed.bindings.empty() && malformed.hid_bindings.empty(),
         "invalid compact HID tokens should remove their complete chords instead of serializing empty IDs or weakening a chord");
}

} // namespace layerc::test
