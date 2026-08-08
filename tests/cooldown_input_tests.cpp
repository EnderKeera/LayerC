#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/input_matcher.hpp"

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
      .stack_charges = true,
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
      .stack_charges = true,
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

} // namespace layerc::test
