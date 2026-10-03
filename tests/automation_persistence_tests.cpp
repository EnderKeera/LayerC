// The shared per-profile automation persistence owner (layer45 F-D1): both
// daemons attach sessions to it, so checkpoint and state-load faults, retry,
// and unsaved values behave identically on Windows and Linux.

#include "layerc/core/automation.hpp"
#include "layerc/platform/automation_persistence.hpp"
#include "layerc/platform/profile_store.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

using layerc::core::AutomationProfile;
using layerc::core::AutomationValue;
using layerc::core::AutomationValueType;
using layerc::core::AutomationVariable;
using layerc::core::TimePoint;
using layerc::test::expect;
using namespace std::chrono_literals;

AutomationProfile counter_profile() {
  AutomationProfile profile;
  profile.profile_id = "game";
  profile.variables.push_back(
      AutomationVariable{.id = "count",
                         .type = AutomationValueType::integer,
                         .default_value = AutomationValue(std::int64_t{0}),
                         .persistent = true});
  return profile;
}

void test_checkpoint_faults_retry_and_recovery() {
  layerc::test::TempDirectory temp{"layerc-automation-persistence"};
  // Isolated store roots for both platform layouts.
  layerc::test::ScopedEnvironment profiles{"LAYERC_PROFILE_DIR",
                                           temp.path() / "profiles"};
  layerc::test::ScopedEnvironment home{"HOME", temp.path()};
  layerc::test::ScopedEnvironment config{"XDG_CONFIG_HOME",
                                         temp.path() / "config"};
  layerc::test::ScopedEnvironment data{"XDG_DATA_HOME", temp.path() / "data"};
  const auto directory = layerc::platform::profile_directory();
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (!expect(!error && !directory.empty(), "an isolated profile store exists"))
    return;
  const auto path = directory / "game.json";
  const auto state_path = layerc::platform::automation_state_path(path);
  const auto profile = counter_profile();
  const TimePoint start = TimePoint{} + 10s;

  layerc::platform::AutomationPersistence owner;
  const auto &fresh = owner.attach(path, profile, start);
  expect(fresh.values.empty() && !fresh.pending() &&
             owner.diagnostics().empty(),
         "a profile without stored state attaches settled and healthy");

  owner.publish(path, {{"count", AutomationValue(std::int64_t{1})}}, start);
  expect(owner.pending_count() == 1 && owner.next_deadline() == start + 1s,
         "a published change arms the shared one-second debounce");
  expect(owner.checkpoint(start + 500ms).empty() && owner.pending_count() == 1,
         "nothing is written before the debounce deadline");

  // A directory where the state file belongs makes every write fail.
  std::filesystem::create_directories(state_path, error);
  if (!expect(!error, "the state destination is faulted")) return;
  for (int attempt = 1; attempt <= 3; ++attempt) {
    const auto now = start + std::chrono::seconds{2 * attempt};
    expect(owner.checkpoint(now).empty() && owner.pending_count() == 1,
           "a failed write keeps the values pending");
    const auto diagnostics = owner.diagnostics();
    expect(diagnostics.size() == 1 &&
               diagnostics.front().starts_with("automation checkpoint failed (") &&
               diagnostics.front().find(path.string()) != std::string::npos,
           "repeated failures report one current checkpoint fault");
    expect(owner.next_deadline() == now + 1s,
           "a failed write retries one debounce later");
  }
  owner.retain({});
  expect(owner.find(path) != nullptr,
         "unsaved values outlive every session until written");

  std::filesystem::remove(state_path, error);
  if (!expect(!error, "the state fault is removed")) return;
  const auto saved = owner.checkpoint(start + 10s, true);
  expect(saved.size() == 1 && saved.front() == path &&
             owner.pending_count() == 0 && owner.diagnostics().empty(),
         "a successful write clears the fault and the pending state");
  const auto persisted = layerc::platform::load_automation_state(path, profile);
  expect(!persisted.error &&
             persisted.values.at("count") == AutomationValue(std::int64_t{1}),
         "the durable store holds the published value");
  owner.retain({});
  expect(owner.find(path) == nullptr,
         "settled entries without a live session are forgotten");

  // A corrupt store is a current load fault until a good write repairs it.
  if (!expect(layerc::test::write_file(state_path, "{invalid"),
              "the stored state is corrupted"))
    return;
  const auto &corrupt = owner.attach(path, profile, start + 20s);
  expect(corrupt.state_load_error.has_value() && corrupt.pending() &&
             owner.diagnostics().size() == 1,
         "an unreadable store reports one load fault and schedules a repair");
  expect(owner.checkpoint(start + 22s).size() == 1 &&
             owner.diagnostics().empty() &&
             !layerc::platform::load_automation_state(path, profile).error,
         "the repair write clears the load fault");

  // Explicit reloads re-read the store only while nothing is unsaved.
  owner.publish(path, {{"count", AutomationValue(std::int64_t{7})}},
                start + 30s);
  const auto &kept = owner.attach(path, profile, start + 30s, true);
  expect(kept.values.at("count") == AutomationValue(std::int64_t{7}),
         "a reload keeps unsaved values");
  auto renamed = profile;
  renamed.variables.front().id = "total";
  const auto &changed = owner.attach(path, renamed, start + 30s);
  expect(changed.pending() && changed.values.empty(),
         "a changed profile keeps only values its variables still accept");
}

} // namespace

int main() {
  return layerc::test::run({
      {"test_checkpoint_faults_retry_and_recovery",
       test_checkpoint_faults_retry_and_recovery},
  });
}
