#include "layerc/core/automation.hpp"
#include "layerc/core/profile.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace layerc::core;

using expr::binary;
using expr::literal;
using expr::variable;

AutomationProfile workload() {
  AutomationProfile profile;
  profile.profile_id = "automation-benchmark";
  profile.variables.push_back(
      {.id = "count",
       .type = AutomationValueType::integer,
       .default_value = AutomationValue(std::int64_t{0})});

  AutomationDefinition automation;
  automation.id = "normal-path";
  automation.name = "Normal path benchmark";
  for (std::size_t index = 0; index < 16; ++index) {
    AutomationRule rule;
    rule.id = "rule-" + std::to_string(index + 1);
    rule.event = AutomationEventKind::input_pressed;
    rule.source_filter = "keyboard:18";
    rule.condition =
        binary("less", variable("count"),
               literal(AutomationValue(std::int64_t{1'000'000'000})));
    AutomationAction action;
    action.kind = AutomationActionKind::increment_variable;
    action.target = "count";
    action.value = literal(AutomationValue(std::int64_t{1}));
    rule.actions.push_back(std::move(action));
    automation.rules.push_back(std::move(rule));
  }
  profile.automations.push_back(std::move(automation));
  return profile;
}

// The base profile the workload graph is admitted against; it names no
// cooldown, panel or group, so the canonical validator checks structure only.
GameProfile workload_base() {
  GameProfile base;
  base.id = "automation-benchmark";
  base.name = "Automation benchmark";
  base.game.executable_name = "automation-benchmark.exe";
  return base;
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

int validate_bundle(const std::filesystem::path &base_path,
                    const std::filesystem::path &automation_path) {
  const auto base_result = parse_profile_json(read_file(base_path));
  const auto *base = std::get_if<GameProfile>(&base_result);
  if (base == nullptr) {
    const auto &error = std::get<ProfileError>(base_result);
    std::cerr << "base " << error.path << ": " << error.message << '\n';
    return 4;
  }
  const auto automation_result =
      parse_automation_profile_json(read_file(automation_path), *base);
  if (const auto *error = std::get_if<AutomationError>(&automation_result)) {
    std::cerr << "automation " << error->path << ": " << error->message << '\n';
    return 5;
  }
  return 0;
}

} // namespace

int main(const int argc, const char *const argv[]) {
  if (argc == 4 && std::string_view(argv[1]) == "--validate")
    return validate_bundle(argv[2], argv[3]);
  constexpr std::size_t warmup_iterations = 2'000;
  constexpr std::size_t measured_iterations = 20'000;
#if defined(NDEBUG)
  constexpr double normal_gate_microseconds = 250.0;
  constexpr double hard_gate_microseconds = 2'000.0;
#else
  // Debug builds include unoptimized iterator assertions and debug checks
  constexpr double normal_gate_microseconds = 500.0;
  constexpr double hard_gate_microseconds = 8'000.0;
#endif
  // Measure real elapsed time below, but keep the runtime's cooperative clock
  // fixed during each event. Scheduler pauses must be samples for the unchanged
  // average/p99 gates, not AR-01 deadline rejections that abort the benchmark.
  // Deterministic deadline/exhaustion coverage lives in the core contract tests.
  TimePoint runtime_now{};
  auto accepted =
      ValidatedAutomationProfile::validate(workload(), workload_base());
  if (const auto *error = std::get_if<AutomationError>(&accepted)) {
    std::cerr << "benchmark workload is not a valid automation profile: "
              << error->describe() << '\n';
    return 6;
  }
  AutomationRuntime runtime(
      std::get<ValidatedAutomationProfile>(std::move(accepted)), {}, {},
      [&] { return runtime_now; });
  AutomationEvent event{.kind = AutomationEventKind::input_pressed,
                        .timestamp = {},
                        .source_id = "keyboard:18",
                        .origin_token = 1,
                        .fields = {}};
  for (std::size_t index = 0; index < warmup_iterations; ++index) {
    event.timestamp = TimePoint(Milliseconds(index));
    runtime_now = event.timestamp;
    event.origin_token = index + 1;
    const auto result = runtime.process(event);
    if (!result.committed) {
      std::cerr << "warmup iteration " << index << " did not commit";
      if (result.error)
        std::cerr << ": " << result.error->path << ": " << result.error->message;
      std::cerr << '\n';
      return 2;
    }
  }

  std::vector<double> samples;
  samples.reserve(measured_iterations);
  for (std::size_t index = 0; index < measured_iterations; ++index) {
    event.timestamp = TimePoint(Milliseconds(warmup_iterations + index));
    runtime_now = event.timestamp;
    event.origin_token = warmup_iterations + index + 1;
    const auto started = MonotonicClock::now();
    const auto result = runtime.process(event);
    const auto elapsed = std::chrono::duration<double, std::micro>(
                             MonotonicClock::now() - started)
                             .count();
    if (!result.committed) {
      std::cerr << "measured iteration " << index << " did not commit";
      if (result.error)
        std::cerr << ": " << result.error->path << ": " << result.error->message;
      std::cerr << '\n';
      return 3;
    }
    samples.push_back(elapsed);
  }

  std::ranges::sort(samples);
  const double average = std::accumulate(samples.begin(), samples.end(), 0.0) /
                         static_cast<double>(samples.size());
  const double p95 = samples[(samples.size() * 95U) / 100U];
  const double p99 = samples[(samples.size() * 99U) / 100U];
  const double maximum = samples.back();
  const bool passed =
      average <= normal_gate_microseconds && p99 <= hard_gate_microseconds;

  std::cout << "{\n"
            << "  \"schemaVersion\": 1,\n"
            << "  \"iterations\": " << measured_iterations << ",\n"
            << "  \"rulesPerEvent\": 16,\n"
            << "  \"averageMicroseconds\": " << average << ",\n"
            << "  \"p95Microseconds\": " << p95 << ",\n"
            << "  \"p99Microseconds\": " << p99 << ",\n"
            << "  \"maxMicroseconds\": " << maximum << ",\n"
            << "  \"normalGateMicroseconds\": " << normal_gate_microseconds
            << ",\n"
            << "  \"hardGateMicroseconds\": " << hard_gate_microseconds << ",\n"
            << "  \"passed\": " << (passed ? "true" : "false") << "\n"
            << "}\n";
  return passed ? 0 : 1;
}
