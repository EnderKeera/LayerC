#include "layerc/core/profile.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_store_linux.hpp"
#include "layerc/platform/profile_transaction.hpp"
#include "profile_transaction_test.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using layerc::test::expect;

enum class DirectoryFault { none, backup, transactions, ancestor, target, commit };
DirectoryFault directory_fault{};
std::filesystem::path fault_target;
std::filesystem::path fault_ancestor;
int failures_remaining{};
int injected_failures{};

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::array<char, 64> pattern{};
    const std::string source = "/tmp/layerc-store-test-XXXXXX";
    std::copy(source.begin(), source.end(), pattern.begin());
    if (char *const result = ::mkdtemp(pattern.data()); result != nullptr)
      path_ = result;
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

std::vector<std::byte> bytes(const std::string_view source) {
  const auto *const first =
      reinterpret_cast<const std::byte *>(source.data());
  return {first, first + source.size()};
}

bool write_file(const std::filesystem::path &path,
                const std::string_view contents) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return false;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return static_cast<bool>(output);
}

std::optional<std::string> read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void test_bounded_file_read_statuses() {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "bounded-read.bin";
  expect(write_file(path, "abcde"), "bounded-read fixture should be written");
  const auto missing = layerc::platform::read_durable_file(
      temporary.path() / "missing.bin", 4U);
  const auto oversized = layerc::platform::read_durable_file(path, 4U);
  const auto successful = layerc::platform::read_durable_file(path, 5U);
  expect(missing.status == layerc::platform::FileReadStatus::missing &&
             oversized.status == layerc::platform::FileReadStatus::too_large &&
             successful.status == layerc::platform::FileReadStatus::ok &&
             successful.bytes.size() == 5U,
         "bounded reads should distinguish missing, oversize, and successful files");
}

std::string profile_json(std::string id) {
  layerc::core::GameProfile profile;
  profile.id = std::move(id);
  profile.name = profile.id;
  profile.game.executable_name = "game.exe";
  profile.cooldowns.push_back(
      {.id = "dash", .name = "Dash", .duration = std::chrono::seconds(5)});
  profile.panels.push_back({.id = "main",
                            .name = "Main",
                            .bar = {},
                            .cooldown_ids = {"dash"}});
  profile.bindings.push_back(
      {.cooldown_id = "dash",
       .chord = {.tokens = {{.device = layerc::core::InputDevice::keyboard,
                             .code = 30}}}});
  return layerc::core::serialize_profile_json(profile);
}

layerc::platform::ProfileStoreRoots roots_under(
    const std::filesystem::path &root) {
  return {.config_profiles = root / "config" / "layerc" / "profiles",
          .data_profiles = root / "data" / "layerc" / "profiles",
          .store = root / "data" / "layerc" / "store"};
}

bool owner_only_directory(const std::filesystem::path &path) {
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
         (status.st_mode & 0777) == 0700 && status.st_uid == ::geteuid();
}

bool owner_only_file(const std::filesystem::path &path) {
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
         (status.st_mode & 0777) == 0600 && status.st_uid == ::geteuid();
}

std::size_t stage_file_count(const std::filesystem::path &directory) {
  std::error_code error;
  std::size_t count{};
  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error))
    if (iterator->path().extension() == ".stage") ++count;
  return error ? 0U : count;
}

void test_xdg_roots_and_permissions() {
  const auto fallback = layerc::platform::profile_store_roots_from(
      "/home/tester", "relative-config", "relative-data");
  expect(fallback.config_profiles ==
             "/home/tester/.config/layerc/profiles" &&
             fallback.data_profiles ==
                 "/home/tester/.local/share/layerc/profiles" &&
             fallback.store == "/home/tester/.local/share/layerc/store",
         "relative XDG roots should use absolute HOME fallbacks");

  const auto overridden = layerc::platform::profile_store_roots_from(
      "/home/tester", "/config", "/data");
  expect(overridden.config_profiles == "/config/layerc/profiles" &&
             overridden.data_profiles == "/data/layerc/profiles" &&
             overridden.store == "/data/layerc/store",
         "absolute XDG roots should be retained");
  expect(layerc::platform::profile_store_roots_from("relative").store.empty(),
         "relative HOME should not produce storage roots");

  TemporaryDirectory temporary;
  const auto roots = roots_under(temporary.path());
  expect(layerc::platform::ensure_profile_store_roots(roots),
         "LayerC XDG directories should be created");
  expect(owner_only_directory(roots.config_profiles.parent_path()) &&
             owner_only_directory(roots.config_profiles) &&
             owner_only_directory(roots.data_profiles.parent_path()) &&
             owner_only_directory(roots.data_profiles) &&
             owner_only_directory(roots.store),
         "LayerC XDG directories should be owner-only");

  const auto outside = temporary.path() / "outside";
  std::error_code error;
  std::filesystem::create_directory(outside, error);
  const auto link = temporary.path() / "linked-config";
  std::filesystem::create_directory_symlink(outside, link, error);
  auto linked_roots = roots;
  linked_roots.config_profiles = link / "profiles";
  expect(!error && !layerc::platform::ensure_profile_store_roots(linked_roots),
         "XDG roots containing symlinks should be rejected");
}

void test_profile_directory_scan_resolves_ids() {
  TemporaryDirectory temporary;
  const auto profiles = temporary.path() / "profiles";
  std::error_code error;
  std::filesystem::create_directories(profiles / "nested", error);
  expect(!error && write_file(profiles / "Friendly Name.JSON",
                              profile_json("actual-id")) &&
             write_file(profiles / "nested" / "ignored.json",
                        profile_json("nested-id")) &&
             write_file(profiles / "broken.json", "not json"),
         "profile scan fixtures should be written");

  const auto scan = layerc::platform::scan_profile_directory(profiles);
  const auto *resolved = scan.find("actual-id");
  expect(scan.error.empty() && !scan.warning.empty() && resolved != nullptr &&
             resolved->path.filename() == "Friendly Name.JSON" &&
             scan.find("nested-id") == nullptr,
         "profile scan should casefold JSON extensions, warn on invalid data, "
         "stay top-level, and resolve parsed ids");

  expect(write_file(profiles / "duplicate.json", profile_json("actual-id")),
         "duplicate profile fixture should be written");
  const auto duplicate = layerc::platform::scan_profile_directory(profiles);
  expect(duplicate.error.find("duplicate profile id 'actual-id'") !=
             std::string::npos,
         "profile scan should reject duplicate parsed ids explicitly");

  const auto not_directory = temporary.path() / "not-a-directory";
  expect(write_file(not_directory, "fixture"),
         "non-directory scan fixture should be written");
  const auto failed = layerc::platform::scan_profile_directory(not_directory);
  expect(!failed.error.empty(),
         "profile scan should report directory enumeration failure");
}

void test_transaction_validation_and_locking() {
  TemporaryDirectory temporary;
  const auto roots = roots_under(temporary.path());
  expect(layerc::platform::ensure_profile_store_roots(roots),
         "transaction roots should be created");

  const layerc::platform::ProfileFileMutation outside = {
      .target = temporary.path() / "outside.json", .contents = bytes("x")};
  expect(!layerc::platform::commit_profile_transaction(roots,
                                                       std::span(&outside, 1)),
         "transaction targets outside config/data roots should be rejected");
  const layerc::platform::ProfileFileMutation traversal = {
      .target = roots.config_profiles / ".." / "escape.json",
      .contents = bytes("x")};
  expect(!layerc::platform::commit_profile_transaction(
             roots, std::span(&traversal, 1)),
         "transaction traversal should be rejected");

  const auto symlink_parent = roots.data_profiles / "linked";
  std::error_code error;
  std::filesystem::create_directory(temporary.path() / "outside-data", error);
  std::filesystem::create_directory_symlink(temporary.path() / "outside-data",
                                            symlink_parent, error);
  const layerc::platform::ProfileFileMutation escaped = {
      .target = symlink_parent / "asset.bin", .contents = bytes("x")};
  expect(!error && !layerc::platform::commit_profile_transaction(
                       roots, std::span(&escaped, 1)),
         "transaction symlink escape should be rejected");

  const auto target = roots.config_profiles / "locked.json";
  const layerc::platform::ProfileFileMutation mutation = {
      .target = target, .contents = bytes("new")};
  const auto lock_path = roots.store / "lock";
  const int lock = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  expect(lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) == 0,
         "test should acquire the store lock");
  expect(!layerc::platform::commit_profile_transaction(roots,
                                                       std::span(&mutation, 1)),
         "a second writer should not bypass the store lock");
  if (lock >= 0) {
    (void)::flock(lock, LOCK_UN);
    ::close(lock);
  }
  expect(owner_only_file(lock_path), "the writer lock should be owner-only");
}

void test_corrupt_journal_self_heals() {
  TemporaryDirectory temporary;
  const auto roots = roots_under(temporary.path());
  expect(layerc::platform::ensure_profile_store_roots(roots),
         "corrupt-journal test directories should be created");

  const auto transaction = roots.store / "transactions" / "corrupt";
  expect(write_file(transaction / "journal.json", "not json"),
         "corrupt journal fixture should be written");
  expect(layerc::platform::recover_profile_transactions(roots),
         "recovery should survive an unparseable journal");
  bool quarantined{};
  for (std::filesystem::directory_iterator iterator(roots.store /
                                                     "transactions"),
       end;
       iterator != end; ++iterator)
    quarantined = quarantined ||
                  iterator->path().filename().string().find(".corrupt-") !=
                      std::string::npos;
  expect(quarantined, "unparseable journals should be quarantined, not "
                       "deleted, preserving the only record of the "
                       "half-published transaction");
  expect(!std::filesystem::exists(transaction),
         "the original corrupt directory should be renamed aside");
  expect(layerc::platform::recover_profile_transactions(roots),
         "a second recovery should leave quarantined evidence alone");

  const auto target = roots.config_profiles / "recovered.json";
  const layerc::platform::ProfileFileMutation mutation = {
      .target = target, .contents = bytes("committed")};
  expect(layerc::platform::commit_profile_transaction(roots,
                                                       std::span(&mutation, 1)) &&
             read_file(target) == "committed",
         "a later transaction should commit after corrupt recovery");
}

void test_precreated_stage_symlink_is_rejected() {
  TemporaryDirectory temporary;
  const auto victim = temporary.path() / "victim";
  const auto stage = temporary.path() / "precreated.stage";
  expect(write_file(victim, "untouched"), "symlink victim should be written");

  std::error_code error;
  std::filesystem::create_symlink(victim, stage, error);
  const auto replacement = bytes("replaced");
  expect(!error &&
             !layerc::platform::create_durable_file(stage, replacement) &&
             read_file(victim) == "untouched",
         "durable create should reject a precreated stage symlink");
}

void test_concurrent_create_only_has_one_winner() {
  TemporaryDirectory temporary;
  const auto roots = roots_under(temporary.path());
  expect(layerc::platform::ensure_profile_store_roots(roots),
         "concurrent-writer test directories should be created");

  int gate[2]{};
  expect(::pipe(gate) == 0, "concurrent writers should create a start gate");
  const auto target = roots.config_profiles / "create-only.json";
  const std::array<std::string_view, 2> contents{"writer-a", "writer-b"};
  std::array<pid_t, 2> children{};
  for (std::size_t index{}; index < children.size(); ++index) {
    children[index] = ::fork();
    if (children[index] == 0) {
      ::close(gate[1]);
      char token{};
      if (::read(gate[0], &token, 1) != 1) _exit(2);
      ::close(gate[0]);
      const layerc::platform::ProfileFileMutation mutation = {
          .target = target,
          .contents = bytes(contents[index]),
          .must_be_absent = true};
      _exit(layerc::platform::commit_profile_transaction(
                roots, std::span(&mutation, 1))
                ? 0
                : 1);
    }
  }
  ::close(gate[0]);
  const char tokens[] = {'a', 'b'};
  const bool started = ::write(gate[1], tokens, sizeof(tokens)) ==
                       static_cast<ssize_t>(sizeof(tokens));
  ::close(gate[1]);

  std::array<int, 2> statuses{};
  bool waited = started;
  for (std::size_t index{}; index < children.size(); ++index)
    waited = children[index] > 0 &&
             ::waitpid(children[index], &statuses[index], 0) ==
                 children[index] &&
             waited;

  std::size_t winners{};
  std::optional<std::size_t> winner;
  for (std::size_t index{}; index < statuses.size(); ++index) {
    if (WIFEXITED(statuses[index]) && WEXITSTATUS(statuses[index]) == 0) {
      ++winners;
      winner = index;
    }
  }
  expect(waited && winners == 1 && winner.has_value() &&
             read_file(target) == contents[*winner],
         "concurrent create-only writers should preserve exactly one winner");
}

void test_cross_root_publish_and_recovery() {
  TemporaryDirectory temporary;
  const auto roots = roots_under(temporary.path());
  expect(layerc::platform::ensure_profile_store_roots(roots),
         "cross-root test directories should be created");

  const auto profile = roots.config_profiles / "game.json";
  const auto visual = roots.data_profiles / "game" / "visual.json";
  const auto automation = roots.data_profiles / "game" / "automation.json";
  const std::array mutations{
      layerc::platform::ProfileFileMutation{.target = profile,
                                            .contents = bytes("new-profile")},
      layerc::platform::ProfileFileMutation{.target = visual,
                                            .contents = bytes("new-visual")},
      layerc::platform::ProfileFileMutation{
          .target = automation, .contents = bytes("new-automation")},
  };

  for (std::size_t publish_count = 1; publish_count <= mutations.size();
       ++publish_count) {
    expect(write_file(profile, "old-profile") &&
               write_file(visual, "old-visual") &&
               write_file(automation, "old-automation"),
           "baseline files should be reset");
    expect(!layerc::platform::linux_testing::
               commit_profile_transaction_fail_after_publish(
                   roots, mutations, publish_count),
           "fault injection should interrupt the transaction");
    expect(std::filesystem::exists(roots.store / "transactions"),
           "faulted transactions should retain a data-root journal");
    if (publish_count < mutations.size())
      expect(read_file(profile) == "old-profile",
             "profile JSON should publish after every data mutation");
    if (publish_count == 1)
      expect(stage_file_count(visual.parent_path()) == 1 &&
                 stage_file_count(profile.parent_path()) == 1,
             "unpublished files should remain staged beside destinations");

    expect(layerc::platform::recover_profile_transactions(roots),
           "faulted cross-root transaction should recover");
    expect(read_file(profile) == "old-profile" &&
               read_file(visual) == "old-visual" &&
               read_file(automation) == "old-automation",
           "recovery should restore the complete prior profile");
    expect(!std::filesystem::exists(roots.store / "transactions"),
           "recovery should remove transaction residue");
  }

  expect(layerc::platform::commit_profile_transaction(roots, mutations),
         "cross-root transaction should commit");
  expect(read_file(profile) == "new-profile" &&
             read_file(visual) == "new-visual" &&
             read_file(automation) == "new-automation",
         "successful transaction should publish data first and profile last");
}

void test_directory_durability_failures() {
  TemporaryDirectory temporary;
  const auto roots = roots_under(temporary.path());
  expect(layerc::platform::ensure_profile_store_roots(roots),
         "durability fixture roots should be created");
  const auto target = roots.config_profiles / "durable.json";
  const layerc::platform::ProfileFileMutation mutation{
      .target = target, .contents = bytes("new")};
  fault_target = target;
  fault_ancestor = roots.store.parent_path();
  const auto inject = [](const DirectoryFault fault, const int remaining = -1) {
    directory_fault = fault;
    failures_remaining = remaining;
    injected_failures = 0;
  };

  for (const auto fault : {DirectoryFault::backup, DirectoryFault::transactions,
                           DirectoryFault::ancestor}) {
    expect(write_file(target, "old"), "reset pre-publication fixture");
    inject(fault);
    const bool committed = layerc::platform::commit_profile_transaction(
        roots, std::span{&mutation, 1});
    directory_fault = DirectoryFault::none;
    expect(!committed && injected_failures > 0 && read_file(target) == "old",
           "a failed backup or ancestor barrier prevents target publication");
    expect(layerc::platform::recover_profile_transactions(roots),
           "a failed preparation remains recoverable");
  }

  expect(write_file(target, "old"), "reset publication fixture");
  inject(DirectoryFault::target);
  const bool committed = layerc::platform::commit_profile_transaction(
      roots, std::span{&mutation, 1});
  directory_fault = DirectoryFault::none;
  expect(!committed && injected_failures >= 2 && read_file(target) == "new" &&
             std::filesystem::exists(roots.store / "transactions"),
         "persistent publication fsync failure reports failure and retains evidence");
  expect(layerc::platform::recover_profile_transactions(roots) &&
             read_file(target) == "old" &&
             layerc::platform::recover_profile_transactions(roots),
         "unacknowledged publication recovers the old document idempotently");

  inject(DirectoryFault::target, 1);
  const bool retried = layerc::platform::commit_profile_transaction(
      roots, std::span{&mutation, 1});
  directory_fault = DirectoryFault::none;
  expect(retried && injected_failures == 1 && read_file(target) == "new" &&
             !std::filesystem::exists(roots.store / "transactions"),
         "a transient publication fault is acknowledged only after a successful retry");

  expect(write_file(target, "old"), "reset commit-marker fixture");
  inject(DirectoryFault::commit);
  const bool marker_confirmed = layerc::platform::commit_profile_transaction(
      roots, std::span{&mutation, 1});
  expect(!marker_confirmed && injected_failures > 0 &&
             std::filesystem::exists(roots.store / "transactions"),
         "an unconfirmed commit marker retains evidence");
  expect(!layerc::platform::recover_profile_transactions(roots) &&
             std::filesystem::exists(roots.store / "transactions"),
         "recovery also retains evidence while the commit flush keeps failing");
  directory_fault = DirectoryFault::none;
  expect(layerc::platform::recover_profile_transactions(roots) &&
             read_file(target) == "new" &&
             layerc::platform::recover_profile_transactions(roots),
         "confirmed committed recovery keeps new bytes and is idempotent");
}

} // namespace

// Linker wrapping is confined to this test executable, with no production
// switches or changes to the actual durable-file implementation.
extern "C" int __real_fsync(int descriptor);
extern "C" int __wrap_fsync(const int descriptor) {
  if (directory_fault != DirectoryFault::none && failures_remaining != 0) {
    struct stat status {};
    if (::fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode)) {
      std::array<char, 4096> buffer{};
      const auto fd_path = "/proc/self/fd/" + std::to_string(descriptor);
      const auto length = ::readlink(fd_path.c_str(), buffer.data(),
                                     buffer.size() - 1);
      if (length > 0) {
        const std::filesystem::path directory{
            std::string(buffer.data(), static_cast<std::size_t>(length))};
        std::error_code error;
        const bool fail =
            (directory_fault == DirectoryFault::backup &&
             directory.filename() == "backup") ||
            (directory_fault == DirectoryFault::transactions &&
             directory.filename() == "transactions") ||
            (directory_fault == DirectoryFault::ancestor &&
             directory == fault_ancestor) ||
            (directory_fault == DirectoryFault::target &&
             directory == fault_target.parent_path() &&
             read_file(fault_target) == "new") ||
            (directory_fault == DirectoryFault::commit &&
             std::filesystem::exists(directory / "committed", error));
        if (fail) {
          ++injected_failures;
          if (failures_remaining > 0) --failures_remaining;
          errno = EIO;
          return -1;
        }
      }
    }
  }
  return __real_fsync(descriptor);
}

int main() {
  return layerc::test::run({
      {"test_xdg_roots_and_permissions", test_xdg_roots_and_permissions},
      {"test_bounded_file_read_statuses", test_bounded_file_read_statuses},
      {"test_profile_directory_scan_resolves_ids", test_profile_directory_scan_resolves_ids},
      {"test_transaction_validation_and_locking", test_transaction_validation_and_locking},
      {"test_corrupt_journal_self_heals", test_corrupt_journal_self_heals},
      {"test_precreated_stage_symlink_is_rejected", test_precreated_stage_symlink_is_rejected},
      {"test_concurrent_create_only_has_one_winner", test_concurrent_create_only_has_one_winner},
      {"test_cross_root_publish_and_recovery", test_cross_root_publish_and_recovery},
      {"test_directory_durability_failures", test_directory_durability_failures},
  });
}
