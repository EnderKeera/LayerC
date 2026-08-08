#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/core/runtime_instances.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace layerc::test {
void test_runtime_instance_tracking() {
  using namespace layerc::core;
  RuntimeInstanceTracker tracker;
  const RuntimeInstanceKey first{.profile_key = L"game-profile", .window = 101};
  const RuntimeInstanceKey second{.profile_key = L"game-profile", .window = 202};
  const RuntimeInstanceKey same_window_other_profile{
      .profile_key = L"game-profile-copy", .window = 202};

  const auto added =
      tracker.reconcile({first, second, same_window_other_profile});
  expect(added.added.size() == 3 && added.removed.empty() &&
             tracker.size() == 3,
         "matching profiles and windows should create independent runtime instances");
  const auto foreground_second = tracker.foreground(202);
  expect(foreground_second.size() == 2 &&
             std::all_of(foreground_second.begin(), foreground_second.end(),
                         [](const RuntimeInstanceKey &key) {
                           return key.window == 202;
                         }),
         "foreground input should route to every profile matching the foreground window");

  const std::vector<CooldownDefinition> cooldowns{{.id = "dash", .name = "Dash",
                                                    .duration = std::chrono::seconds(5)}};
  CooldownEngine first_engine(cooldowns);
  CooldownEngine second_engine(cooldowns);
  expect(first_engine.trigger("dash", TimePoint{}),
         "first instance should accept its own cooldown trigger");
  expect(second_engine.trigger("dash", TimePoint{} + std::chrono::seconds(2)),
         "second instance should accept a separate later trigger");
  expect(first_engine.remaining("dash", TimePoint{} + std::chrono::seconds(2)) ==
             std::chrono::seconds(3) &&
             second_engine.remaining("dash", TimePoint{} + std::chrono::seconds(2)) ==
                 std::chrono::seconds(5),
         "two matching instances should retain separate cooldown timelines");

  const auto removed = tracker.reconcile({second, same_window_other_profile});
  expect(removed.removed.size() == 1 && removed.removed.front() == first &&
             tracker.size() == 2 && tracker.foreground(101).empty(),
         "closing or minimizing a matched window should retire its runtime instance");

  const auto launcher_exit = tracker.reconcile({});
  expect(launcher_exit.removed.size() == 2 && tracker.size() == 0,
         "a launcher transition with no matching windows should retire all instances");
  const RuntimeInstanceKey restarted{.profile_key = L"game-profile",
                                     .window = 303};
  const auto game_restart = tracker.reconcile({restarted});
  expect(game_restart.added == std::vector<RuntimeInstanceKey>{restarted} &&
             tracker.foreground(303) ==
                 std::vector<RuntimeInstanceKey>{restarted},
         "a restarted game process should create a fresh routable instance");
  RuntimeInstanceTracker daemon_restart;
  expect(daemon_restart.reconcile({restarted}).added ==
             std::vector<RuntimeInstanceKey>{restarted},
         "a daemon restart should reconstruct instances from current matches");
}
void test_json_unicode_and_errors() {
  using namespace layerc::core::json;

  auto result = parse(R"json({"name":"Layer\u0043","value":1.5})json");
  expect(std::holds_alternative<Value>(result),
         "valid unicode JSON should parse");
  if (const auto *value = std::get_if<Value>(&result)) {
    expect(value->find("name") != nullptr &&
               value->find("name")->as_string() == "LayerC",
           "unicode escape should decode to UTF-8");
  }

  result = parse("[1,]");
  expect(std::holds_alternative<ParseError>(result),
         "trailing array comma should fail");
  const auto leading_zero = parse(R"({"value":01})");
  const auto *leading_zero_error = std::get_if<ParseError>(&leading_zero);
  expect(leading_zero_error != nullptr && leading_zero_error->offset == 10,
         "RFC 8259 leading-zero numbers should fail at the extra digit");
}

void test_json_nesting_limit() {
  using namespace layerc::core::json;

  std::string excessive(200, '[');
  excessive += '0';
  excessive.append(200, ']');
  expect(std::holds_alternative<ParseError>(parse(excessive)),
         "excessively nested JSON arrays should return a parse error");

  std::string excessive_objects;
  for (int depth = 0; depth < 200; ++depth) excessive_objects += R"json({"x":)json";
  excessive_objects += '0';
  excessive_objects.append(200, '}');
  expect(std::holds_alternative<ParseError>(parse(excessive_objects)),
         "excessively nested JSON objects should return a parse error");

  std::string ordinary(32, '[');
  ordinary += '0';
  ordinary.append(32, ']');
  expect(std::holds_alternative<Value>(parse(ordinary)),
         "ordinary nested JSON should remain supported");
}

void test_json_malformed_corpus() {
  using namespace layerc::core;
  std::uint32_t state = 0xC0FFEEU;
  std::size_t rejected{};
  std::size_t survived_as_valid{};
  const std::string seed =
      R"json({"schemaVersion":1,"id":"game","name":"Game","game":{"executableName":"game.exe"},"cooldowns":[{"id":"dash","name":"Dash","durationMs":1000}],"panels":[{"id":"main","name":"Main","cooldowns":["dash"]}],"bindings":[{"cooldownId":"dash","chord":[{"device":"keyboard","code":69}]}]})json";
  for (int iteration = 0; iteration < 512; ++iteration) {
    auto candidate = seed;
    state = state * 1664525U + 1013904223U;
    const std::size_t changes = 1U + state % 8U;
    for (std::size_t change = 0; change < changes; ++change) {
      state = state * 1664525U + 1013904223U;
      const auto offset = static_cast<std::size_t>(state) % candidate.size();
      candidate[offset] = static_cast<char>((state >> 16U) & 0xFFU);
    }
    const auto result = parse_profile_json(candidate);
    if (const auto *profile = std::get_if<GameProfile>(&result)) {
      ++survived_as_valid;
      expect(std::holds_alternative<GameProfile>(
                 parse_profile_json(serialize_profile_json(*profile))),
             "valid corpus survivors should remain serializable");
    } else {
      ++rejected;
    }
  }
  expect(rejected > 450 && rejected + survived_as_valid == 512,
         "the deterministic corrupt-profile corpus should be rejected safely");
}

} // namespace layerc::test
