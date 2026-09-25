#include "platform_live_test_support.hpp"

#include "layerc/platform/bounded_helper.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/visual_assets_windows.hpp"
#include "test_support.hpp"

#include <chrono>
#include <utility>
#include <vector>

namespace layerc::test::platform_live {

core::GameProfile test_profile(const std::string &name) {
  using namespace std::chrono_literals;
  core::GameProfile profile;
  profile.id = "store-test";
  profile.name = name;
  profile.game.executable_name = "test.exe";
  profile.cooldowns.push_back(
      {.id = "dash", .name = "Dash", .duration = 1s});
  profile.panels.push_back(
      {.id = "main", .name = "Main", .bar = {}, .cooldown_ids = {"dash"}});
  profile.bindings.push_back(
      {.cooldown_id = "dash",
       .chord = {{{core::InputDevice::keyboard, 0x20, 0}}}});
  return profile;
}

core::VisualProfile test_visual_profile() {
  core::VisualProfile profile;
  profile.profile_id = "store-test";
  profile.rules.push_back({.id = "ready",
                           .name = "Ready",
                           .region = {0.1, 0.2, 0.3, 0.4},
                           .predicate = core::AverageColorPredicate{0x123456,
                                                                   10}});
  profile.triggers.push_back({.cooldown_id = "dash",
                              .observation_id = "ready",
                              .mode = core::VisualTriggerMode::pixel_only});
  return profile;
}

core::AutomationProfile
test_automation_profile(const std::string &profile_id) {
  core::AutomationProfile profile;
  profile.profile_id = profile_id;
  profile.variables.push_back(
      {.id = "count",
       .type = core::AutomationValueType::integer,
       .default_value = core::AutomationValue(std::int64_t{0}),
       .persistent = true});
  core::AutomationRule rule;
  rule.id = "start";
  rule.event = core::AutomationEventKind::session_started;
  core::AutomationAction action;
  action.kind = core::AutomationActionKind::increment_variable;
  action.target = "count";
  action.value = core::expr::literal(core::AutomationValue(std::int64_t{1}));
  rule.actions.push_back(std::move(action));
  profile.automations.push_back(
      {.id = "counter", .name = "Counter", .rules = {rule}});
  return profile;
}

bool launch_auth_peer(const std::filesystem::path &executable,
                      const wchar_t *const mode, const std::wstring &pipe_name,
                      PROCESS_INFORMATION &process) {
  std::wstring command =
      platform::quote_command_argument(executable.wstring()) + L" " + mode +
      L" " + platform::quote_command_argument(pipe_name);
  std::vector<wchar_t> command_buffer(command.begin(), command.end());
  command_buffer.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  return CreateProcessW(executable.c_str(), command_buffer.data(), nullptr,
                        nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process) != FALSE;
}

bool durable_read_contract(const std::filesystem::path &directory) {
  const auto absent =
      platform::read_durable_file(directory / L"absent.bin", 4U);
  const auto path = directory / L"bounded-read.bin";
  write_file(path, "abcde");
  const auto oversized = platform::read_durable_file(path, 4U);
  const auto successful = platform::read_durable_file(path, 5U);
  return absent.status == platform::FileReadStatus::missing &&
         oversized.status == platform::FileReadStatus::too_large &&
         successful.status == platform::FileReadStatus::ok &&
         successful.bytes.size() == 5U;
}

bool completed_visual_gc(const std::size_t minimum_deleted,
                         const std::size_t minimum_retained) {
  const auto report = platform::garbage_collect_unreferenced_visual_assets();
  return report.completed() && report.deleted >= minimum_deleted &&
         report.retained >= minimum_retained;
}

bool visual_gc_blocked_by(const std::filesystem::path &path) {
  const auto report = platform::garbage_collect_unreferenced_visual_assets();
  return !report.completed() && report.blocked_by == path &&
         !report.fault.empty();
}

} // namespace layerc::test::platform_live
