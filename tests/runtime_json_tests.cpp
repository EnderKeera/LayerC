#include "core_test_cases.hpp"
#include "test_support.hpp"

#include "layerc/core/cooldown_engine.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/runtime_instances.hpp"

#include <algorithm>
#include <chrono>
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
  expect(first_engine.snapshot(TimePoint{}).front().next_charge_at.has_value() &&
             !second_engine.snapshot(TimePoint{}).front().next_charge_at.has_value(),
         "two matching instances should retain separate cooldown timelines");

  const auto removed = tracker.reconcile({second, same_window_other_profile});
  expect(removed.removed.size() == 1 && removed.removed.front() == first &&
             tracker.size() == 2 && tracker.foreground(101).empty(),
         "closing or minimizing a matched window should retire its runtime instance");
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

} // namespace layerc::test
