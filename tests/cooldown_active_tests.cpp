// Standalone cooldown active windows (profile activeDurationMs): a second
// per-public-ID clock armed only by accepted triggers.

#include "automation_test_support.hpp"
#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/automation_session.hpp"
#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/live_edit.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace layerc::test {
namespace {
using namespace layerc::core;
using namespace std::chrono_literals;

const CooldownState &state_of(const std::vector<CooldownState> &states,
                              const std::string_view id) {
  for (const auto &state : states)
    if (state.id == id) return state;
  static const CooldownState missing{};
  return missing;
}

std::optional<TimePoint> active_until(CooldownEngine &engine,
                                      const std::string_view id,
                                      const TimePoint now) {
  return state_of(engine.snapshot(now), id).active_until;
}

CooldownDefinition cooldown(std::string id, const Milliseconds duration,
                            const Milliseconds active,
                            const RetriggerPolicy retrigger = RetriggerPolicy::ignore,
                            const std::uint32_t charges = 1,
                            const RecoveryMode recovery = RecoveryMode::single) {
  CooldownDefinition definition;
  definition.id = id;
  definition.name = std::move(id);
  definition.duration = duration;
  definition.max_charges = charges;
  definition.retrigger = retrigger;
  definition.recovery = recovery;
  definition.active_duration = active;
  return definition;
}

CooldownGroup group(std::string id, const CooldownGroupMode mode,
                    std::vector<std::string> members) {
  CooldownGroup result;
  result.id = id;
  result.name = std::move(id);
  result.mode = mode;
  result.cooldown_ids = std::move(members);
  result.duration = 10s;
  result.max_charges = 1;
  return result;
}
} // namespace

void test_cooldown_active_windows() {
  const auto start = TimePoint{} + 100s;

  {
    const std::vector definitions{cooldown("dash", 5s, 2s)};
    CooldownEngine engine(definitions);
    expect(!active_until(engine, "dash", start).has_value(),
           "no trigger means no active window");
    expect(engine.trigger("dash", start), "first trigger is accepted");
    const auto snapshot = engine.snapshot(start);
    expect(state_of(snapshot, "dash").active_until == start + 2s &&
               state_of(snapshot, "dash").active_duration == 2s,
           "an accepted trigger arms the window for the active duration");
    expect(active_until(engine, "dash", start + 2s - 1ms) == start + 2s,
           "the window stays until its deadline");
    expect(!engine.trigger("dash", start + 1s),
           "an ignore retrigger while recharging is rejected");
    expect(active_until(engine, "dash", start + 1s) == start + 2s,
           "a rejected trigger never touches the window");
    expect(!active_until(engine, "dash", start + 2s).has_value(),
           "exact expiry is inactive");
  }

  {
    const std::vector definitions{
        cooldown("dash", 5s, 2s, RetriggerPolicy::restart)};
    CooldownEngine engine(definitions);
    expect(engine.trigger("dash", start), "restart: first trigger");
    expect(engine.trigger("dash", start + 1s), "restart retrigger is accepted");
    expect(active_until(engine, "dash", start + 1s) == start + 3s,
           "an accepted restart restarts the window");
  }

  {
    const std::vector definitions{cooldown("dash", 5s, 2s, RetriggerPolicy::ignore,
                                           3, RecoveryMode::parallel)};
    CooldownEngine engine(definitions);
    expect(engine.trigger("dash", start) && engine.trigger("dash", start + 1s),
           "two charges are accepted");
    expect(active_until(engine, "dash", start + 1s) == start + 3s,
           "a second charge refreshes one window instead of stacking");
  }

  {
    const std::vector definitions{cooldown("dash", 2s, 6s)};
    CooldownEngine engine(definitions);
    expect(engine.trigger("dash", start), "long active window: trigger");
    const auto later = engine.snapshot(start + 3s);
    expect(cooldown_ready(state_of(later, "dash")) &&
               state_of(later, "dash").active_until == start + 6s,
           "an active window may outlive the recharge");
    (void)engine.refund("dash", start + 3s);
    expect(active_until(engine, "dash", start + 3s) == start + 6s,
           "refund does not alter activity");
  }

  {
    const std::vector definitions{cooldown("dash", 5s, 0ms)};
    CooldownEngine engine(definitions);
    expect(engine.trigger("dash", start) &&
               !active_until(engine, "dash", start).has_value(),
           "a zero active duration never arms");
  }

  {
    const std::vector definitions{cooldown("a", 5s, 2s), cooldown("b", 5s, 3s),
                                  cooldown("c", 5s, 4s)};
    const std::vector groups{
        group("chain", CooldownGroupMode::trigger_propagation, {"a", "b"})};
    CooldownEngine engine(definitions, groups);
    const auto triggered = engine.trigger_with_propagation("a", start, 7);
    expect(triggered.size() == 2, "propagation accepts both members");
    const auto snapshot = engine.snapshot(start);
    expect(state_of(snapshot, "a").active_until == start + 2s &&
               state_of(snapshot, "b").active_until == start + 3s &&
               !state_of(snapshot, "c").active_until.has_value(),
           "each accepted propagated ID arms its own duration");
    expect(engine.trigger_with_propagation("b", start + 1s, 7).empty(),
           "one root origin triggers each owner once");
    expect(active_until(engine, "b", start + 1s) == start + 3s,
           "a claimed origin does not re-arm");
    (void)engine.reduce("a", 1s, false, start + 1s);
    expect(active_until(engine, "a", start + 1s) == start + 2s,
           "reduction does not alter activity");
  }

  {
    const std::vector definitions{cooldown("a", 5s, 2s), cooldown("b", 5s, 3s)};
    const std::vector groups{
        group("pool", CooldownGroupMode::shared_pool, {"a", "b"})};
    CooldownEngine engine(definitions, groups);
    expect(engine.trigger("a", start), "pooled member trigger");
    auto snapshot = engine.snapshot(start);
    expect(state_of(snapshot, "a").active_until == start + 2s &&
               !state_of(snapshot, "b").active_until.has_value() &&
               !cooldown_ready(state_of(snapshot, "b")),
           "a pooled trigger mirrors recharge but arms only the triggered ID");
    expect(engine.reset("b"), "reset through the other alias");
    snapshot = engine.snapshot(start);
    expect(!state_of(snapshot, "a").active_until.has_value() &&
               cooldown_ready(state_of(snapshot, "a")),
           "a pool reset clears every member's window");
    const auto members = engine.trigger_group("pool", start + 1s);
    snapshot = engine.snapshot(start + 1s);
    expect(members.size() == 2 &&
               state_of(snapshot, "a").active_until == start + 3s &&
               state_of(snapshot, "b").active_until == start + 4s,
           "triggering the pool arms every member");
    expect(engine.reset_group("pool"), "group reset");
    snapshot = engine.snapshot(start + 1s);
    expect(!state_of(snapshot, "a").active_until.has_value() &&
               !state_of(snapshot, "b").active_until.has_value(),
           "a group reset clears all affected windows");
  }

  {
    const std::vector definitions{cooldown("dash", 5s, 2s)};
    CooldownEngine engine(definitions);
    expect(engine.trigger("dash", start), "standalone reset: trigger");
    expect(engine.reset("dash") && !active_until(engine, "dash", start).has_value(),
           "an explicit reset clears a standalone window");
  }

  {
    const std::vector definitions{cooldown("dash", 5s, 2s), cooldown("blink", 5s, 2s)};
    CooldownEngine engine(definitions);
    expect(engine.trigger("dash", start) && engine.trigger("blink", start),
           "reconfigure: both armed");
    auto renamed = definitions;
    renamed[0].name = "Dash!";
    engine.reconfigure(renamed, {}, start + 1s);
    expect(active_until(engine, "dash", start + 1s) == start + 2s,
           "a name-only edit keeps the window");
    auto changed = renamed;
    changed[0].active_duration = 3s;
    changed[1].duration = 6s;
    engine.reconfigure(changed, {}, start + 1s);
    const auto snapshot = engine.snapshot(start + 1s);
    expect(!state_of(snapshot, "dash").active_until.has_value() &&
               !state_of(snapshot, "blink").active_until.has_value(),
           "a changed active duration or recharge setting clears the window");
    expect(engine.trigger("dash", start + 6s), "re-arm after the recharge");
    const std::array<std::string, 1> reset{"dash"};
    engine.reconfigure(changed, reset, start + 6s);
    expect(!active_until(engine, "dash", start + 6s).has_value(),
           "an explicit reconfigure reset clears the window");
    engine.reconfigure(std::vector{changed[1]}, {}, start + 6s);
    expect(engine.snapshot(start + 6s).size() == 1, "a removed ID disappears");
  }

  {
    const std::vector definitions{cooldown("dash", 5s, 1s)};
    CooldownEngine engine(definitions);
    const auto edge = TimePoint::max() - 500ms;
    expect(!engine.can_arm_active(edge) && !engine.trigger("dash", edge) &&
               engine.trigger_with_propagation("dash", edge).empty(),
           "an unrepresentable active deadline rejects the trigger");
    const auto snapshot = engine.snapshot(edge);
    expect(cooldown_ready(state_of(snapshot, "dash")) &&
               state_of(snapshot, "dash").available_charges == 1 &&
               !state_of(snapshot, "dash").active_until.has_value(),
           "the rejected trigger left no charge or activity mutation");
  }

  {
    CooldownState state;
    state.max_charges = 2;
    state.available_charges = 1;
    state.next_charge_at = start + 2'901ms;
    const CooldownDefinition dash = cooldown("dash", 5s, 0ms);
    expect(cooldown_status_text(dash, state, start) == "dash  3.0s  [1]" &&
               cooldown_status_value(state, start) == "3.0s  [1]" &&
               format_tenths(0ms) == "0.0s" && format_tenths(1ms) == "0.1s",
           "status text bytes are unchanged");
  }
}

void test_cooldown_active_session_and_diff() {
  GameProfile profile;
  profile.id = "game";
  profile.name = "Game";
  profile.game.executable_name = "game.exe";
  profile.cooldowns.push_back(cooldown("dash", 5s, 1s));
  Panel panel;
  panel.id = "main";
  panel.name = "Main";
  panel.cooldown_ids = {"dash"};
  profile.panels.push_back(panel);

  AutomationProfile automation;
  automation.profile_id = "game";
  AutomationRule rule;
  rule.id = "press";
  rule.event = AutomationEventKind::input_pressed;
  rule.actions.push_back(
      {.kind = AutomationActionKind::trigger_cooldown, .target = "dash"});
  automation.automations.push_back(
      {.id = "auto", .name = "Auto", .enabled = true, .rules = {rule}});

  const auto start = TimePoint{} + 10s;
  const auto edge = TimePoint::max() - 500ms;
  AutomationEvent pressed{.kind = AutomationEventKind::input_pressed,
                          .timestamp = edge,
                          .source_id = "keyboard:1",
                          .origin_token = 41,
                          .fields = {}};
  {
    AutomationSession overflow(profile);
    (void)overflow.start(accept_automation(automation, profile), {}, start,
                         nullptr, false);
    expect(!overflow.process(pressed, nullptr),
           "an unrepresentable active window commits nothing");
    const auto after_overflow = overflow.snapshot(edge);
    expect(cooldown_ready(after_overflow.front()) &&
               after_overflow.front().available_charges == 1 &&
               !after_overflow.front().active_until.has_value(),
           "session admission rejects the whole batch on active overflow");
  }
  AutomationSession session(profile);
  (void)session.start(accept_automation(automation, profile), {}, start,
                      nullptr, false);
  pressed.timestamp = start + 1s;
  pressed.origin_token = 42;
  (void)session.process(pressed, nullptr);
  const auto armed = session.snapshot(start + 1s);
  expect(!cooldown_ready(armed.front()) &&
             armed.front().active_until == start + 2s,
         "an automation trigger arms the active window");
  expect(session.presentation_snapshot() != nullptr,
         "a running session exposes its presentation snapshot");

  auto renamed = profile;
  renamed.cooldowns.front().name = "Dash!";
  auto longer = profile;
  longer.cooldowns.front().active_duration = 2s;
  expect(diff_profiles(profile, renamed).cooldown_visuals_changed &&
             diff_profiles(profile, longer).cooldown_visuals_changed &&
             diff_profiles(profile, longer).reset_cooldown_ids.empty(),
         "active-duration edits rebuild overlays without resetting recharge");
  expect(!diff_profiles(profile, profile).cooldown_visuals_changed,
         "unchanged profiles report no visual change");

  automation.variables.push_back({.id = "stacks",
                                  .type = AutomationValueType::integer,
                                  .default_value = AutomationValue(std::int64_t{3}),
                                  .persistent = false});
  const auto defaults = automation_default_snapshot(automation);
  expect(defaults.variables.size() == 1 && defaults.timers.empty() &&
             defaults.overlay_overrides.empty() &&
             defaults.variables.at("stacks") == AutomationValue(std::int64_t{3}),
         "the default snapshot holds declared defaults only");
}

} // namespace layerc::test
