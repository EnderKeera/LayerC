#include "platform_live_test_support.hpp"

#include "layerc/platform/bounded_helper.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_file_guard.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/visual_assets_windows.hpp"
#include "layerc/platform/win32_handle.hpp"
#include "../src/platform/linux/profile_transaction_test.hpp"
#include "test_support.hpp"

#include <array>
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
  profile.observations.push_back(layerc::core::PixelRule{.id = "ready",
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

void journal_read_status_recovery() {
  constexpr std::size_t limit = 256U * 1024U;
  constexpr std::string_view valid_journal =
      "{\"schemaVersion\":1,\"operations\":[{\"target\":\"target.bin\","
      "\"existed\":true}]}\n";
  for (const auto status : {platform::FileReadStatus::missing,
                            platform::FileReadStatus::too_large,
                            platform::FileReadStatus::io_error,
                            platform::FileReadStatus::ok}) {
    TempDirectory temp("LayerC-journal-status");
    const auto roots = platform::profile_transaction_scope(temp.path());
    if (!expect(roots.has_value(), "journal fixture should resolve its scope"))
      continue;
    const auto transaction = roots->store /
        platform::transaction_directory_name() / "read-status";
    const auto journal = transaction / "journal.json";
    const auto backup = transaction / "backup" / "0.bin";
    const auto target = temp.path() / "target.bin";
    if (!expect(write_file(backup, "old") && write_file(target, "partial") &&
                    write_file(transaction / "published", "1\n"),
                "journal status fixture should keep disposable rollback bytes"))
      continue;
    if (status != platform::FileReadStatus::missing)
      expect(write_file(journal, status == platform::FileReadStatus::too_large
                                     ? std::string(limit + 1U, ' ')
                                     : std::string(valid_journal)),
             "journal fixture should be written");
    platform::OwnedHandle blocked;
    if (status == platform::FileReadStatus::io_error) {
      blocked.reset(CreateFileW(journal.c_str(), GENERIC_READ, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
      expect(blocked.valid(), "journal fixture should deny concurrent reads");
    }
    expect(platform::read_durable_file(journal, limit).status == status,
           "recovery fixture should exercise the exact typed read status");
    const auto recovered = platform::recover_profile_transactions(*roots);
    if (status == platform::FileReadStatus::too_large) {
      const std::filesystem::path quarantined(transaction.string() + ".corrupt-0");
      std::error_code size_error;
      const auto journal_size =
          std::filesystem::file_size(quarantined / "journal.json", size_error);
      expect(recovered && !std::filesystem::exists(transaction) && !size_error &&
                 journal_size == limit + 1U &&
                 read_file(quarantined / "backup" / "0.bin") == "old" &&
                 read_file(target) == "partial",
             "oversized journals are quarantined with all rollback evidence");
      expect(platform::recover_profile_transactions(*roots) &&
                 std::filesystem::exists(quarantined),
             "repeated recovery must leave quarantined evidence alone");
      const std::array mutation{platform::ProfileFileMutation{
          .target = target, .contents = core::owned_bytes("after-quarantine")}};
      expect(platform::commit_profile_transaction(*roots, mutation) &&
                 read_file(target) == "after-quarantine",
             "a quarantined oversized journal must not block later publications");
    } else if (status == platform::FileReadStatus::io_error) {
      expect(recovered.error() == platform::ProfileWriteError::recovery_blocked &&
                 read_file(backup) == "old" && std::filesystem::exists(journal) &&
                 read_file(target) == "partial",
             "existing unreadable journals must retain all evidence");
      expect(platform::recover_profile_transactions(*roots).error() ==
                     platform::ProfileWriteError::recovery_blocked &&
                 read_file(backup) == "old",
             "repeated recovery must not degrade unreadable journal evidence");
      const std::array mutation{platform::ProfileFileMutation{
          .target = target, .contents = core::owned_bytes("must-not-publish")}};
      expect(platform::commit_profile_transaction(*roots, mutation).error() ==
                     platform::ProfileWriteError::recovery_blocked &&
                 read_file(target) == "partial",
             "blocked recovery must prevent further publications");
      blocked.reset();
      expect(write_file(journal, valid_journal) &&
                 platform::recover_profile_transactions(*roots) &&
                 read_file(target) == "old" && !std::filesystem::exists(transaction),
             "a readable current-release journal should recover retained evidence");
    } else {
      expect(recovered && !std::filesystem::exists(transaction) &&
                 read_file(target) ==
                     (status == platform::FileReadStatus::missing ? "partial" : "old"),
             "only missing journals permit pre-journal cleanup; ok journals roll back");
    }
  }
}

void profile_planning_after_recovery() {
  TempDirectory temp("LayerC-owned-planning");
  ScopedEnvironment profile_dir("LAYERC_PROFILE_DIR", temp.path());
  if (!expect(profile_dir.applied(), "planning must use an isolated profile store"))
    return;
  const auto roots = platform::profile_transaction_scope(temp.path());
  if (!expect(roots.has_value(), "planning fixture should resolve its scope")) return;
  const auto path = temp.path() / "bundle.json";
  auto committed = test_profile("Last committed bundle");
  const auto visual = test_visual_profile();
  const auto automation = test_automation_profile();
  core::CompanionProfile companion;
  companion.profile_id = committed.id;
  companion.slots.push_back({.slot_index = 0, .panel_id = "main", .opacity = 0.9});
  if (!expect(platform::write_profile_bundle(path, committed, visual, automation,
                                             companion),
              "latest daemon preferences should be committed first")) return;
  const std::array paths{path, platform::visual_sidecar_path(path),
                        platform::automation_sidecar_path(path),
                        platform::companion_sidecar_path(path)};
  std::array<std::string, 4> preceding;
  for (std::size_t index{}; index < paths.size(); ++index)
    preceding[index] = read_file(paths[index]).value_or("");
  auto interrupted = committed;
  interrupted.name = "Interrupted writer";
  auto stale_companion = companion;
  stale_companion.slots.front().opacity = 0.2;
  auto stale_visual = visual;
  stale_visual.sample_rate_hz = 5;
  auto stale_automation = automation;
  stale_automation.variables.front().default_value = core::AutomationValue(std::int64_t{8});
  const std::array contents{
      core::serialize_profile_json(interrupted),
      core::serialize_visual_profile_json(stale_visual),
      core::serialize_automation_profile_json(stale_automation),
      core::serialize_companion_profile_json(stale_companion)};
  std::vector<platform::ProfileFileMutation> partial;
  for (std::size_t index{}; index < paths.size(); ++index)
    partial.push_back({.target = paths[index], .contents = core::owned_bytes(contents[index])});
  // Deterministic writer/recovery interleaving: leave all primaries partially
  // published, then have the editor save while omitting companion preferences.
  expect(!platform::linux_testing::commit_profile_transaction_fail_after_publish(
             *roots, partial, partial.size()),
         "simulated writer loss should leave a real rollback journal");
  auto edited = committed;
  edited.name = "Editor edit";
  expect(platform::write_profile_bundle(path, edited, visual, automation),
         "the editor save should recover before preserving omitted sidecars");
  const auto preserved = platform::load_companion_sidecar(path, edited);
  expect(preserved.profile == companion,
         "omitted companion preferences must preserve the latest committed daemon value");
  for (std::size_t index{}; index < paths.size(); ++index)
    expect(read_file(platform::recovery_backup_path(paths[index])) == preceding[index],
           "bundle backups must describe the immediately preceding recovered commit");

  // The single-file writer uses the identical ownership boundary for its .bak.
  const auto previous = read_file(path).value_or("");
  const std::array partial_base{platform::ProfileFileMutation{
      .target = path, .contents = core::owned_bytes(contents.front())}};
  expect(!platform::linux_testing::commit_profile_transaction_fail_after_publish(
             *roots, partial_base, 1), "single-file partial publish should interrupt");
  edited.name = "Next editor edit";
  expect(platform::write_profile_file(path, core::serialize_profile_json(edited)) &&
             read_file(platform::recovery_backup_path(path)) == previous,
         "single-file backup planning must run after recovery, not before it");
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
