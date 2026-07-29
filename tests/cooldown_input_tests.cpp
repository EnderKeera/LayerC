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
  engine.snapshot(TimePoint{}, states);
  const auto capacity = states.capacity();
  const auto *storage = states.data();
  engine.snapshot(TimePoint{} + 1s, states);
  expect(states.capacity() == capacity && states.data() == storage,
         "cooldown snapshots should reuse their caller-owned buffer");
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

} // namespace layerc::test
