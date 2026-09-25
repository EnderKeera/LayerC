#include "layerc/platform/profile_transaction.hpp"

#include "layerc/core/json.hpp"
#include "layerc/platform/durable_file.hpp"
#include "layerc/platform/profile_file_guard.hpp"
#include "layerc/platform/profile_store.hpp"
#include "layerc/platform/text.hpp"

#include "profile_transaction_internal.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace layerc::platform {
namespace {

constexpr std::size_t max_transaction_bytes = 128U * 1024U * 1024U;
constexpr std::size_t max_transaction_operations = 512U;
constexpr std::size_t max_journal_bytes = 256U * 1024U;
std::atomic_uint64_t transaction_sequence{};

enum class RootKind { config, data };

struct JournalOperation {
  RootKind root{};
  std::filesystem::path relative;
  std::filesystem::path target;
  std::filesystem::path stage;
  bool existed{};
  bool must_be_absent{};
  std::optional<std::vector<std::byte>> contents;
};

bool valid_relative(const std::filesystem::path &relative) {
  if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
      relative.has_root_directory())
    return false;
  // The journal directory nests below the target roots on unified stores,
  // so its name is reserved there; split stores keep it outside both roots
  // and reserve nothing. The guard owns the topology, not this loop.
  bool first = true;
  for (const auto &part : relative) {
    const auto name = path_to_utf8(part);
    if (!name.has_value() || name->empty() || *name == "." || *name == ".." ||
        name->size() > 255U ||
        std::ranges::any_of(*name, [](const char character) {
          return static_cast<unsigned char>(character) < 0x20U;
        }))
      return false;
    if (first && transaction_reserved_first_component(*name)) return false;
    first = false;
  }
  return true;
}

std::optional<std::filesystem::path>
relative_target(const std::filesystem::path &root,
                const std::filesystem::path &target) {
  if (!root.is_absolute() || !target.is_absolute()) return std::nullopt;
  const auto relative = target.lexically_normal().lexically_relative(
      root.lexically_normal());
  return valid_relative(relative) ? std::optional(relative) : std::nullopt;
}

bool ensure_target_parent(const std::filesystem::path &root,
                          const std::filesystem::path &relative) {
  if (!file_guard_owned_directory(root) || file_guard_is_symlink(root))
    return false;
  auto current = root;
  for (const auto &part : relative.parent_path()) {
    current /= part;
    if (!file_guard_ensure_directory(current)) return false;
  }
  return true;
}

bool safe_target(const std::filesystem::path &root,
                 const std::filesystem::path &relative) {
  if (!ensure_target_parent(root, relative)) return false;
  return file_guard_absent_or_regular(root / relative);
}

bool durable_write_new(const std::filesystem::path &path,
                       const std::span<const std::byte> bytes) {
  return file_guard_owned_directory(path.parent_path()) &&
         !file_guard_is_symlink(path.parent_path()) &&
         create_durable_file(path, bytes);
}

bool durable_write_replacing(const std::filesystem::path &path,
                             const std::span<const std::byte> bytes) {
  if (!file_guard_owned_directory(path.parent_path()) ||
      file_guard_is_symlink(path.parent_path())) return false;
  return write_durable_file(path, bytes) ==
         DurablePublishOutcome::published_durable;
}

bool durable_write_new(const std::filesystem::path &path,
                       const std::string_view text) {
  return durable_write_new(
      path, std::as_bytes(std::span(text.data(), text.size())));
}

bool durable_write_replacing(const std::filesystem::path &path,
                             const std::string_view text) {
  return durable_write_replacing(
      path, std::as_bytes(std::span(text.data(), text.size())));
}

bool copy_durable(const std::filesystem::path &source,
                  const std::filesystem::path &destination) {
  const auto bytes = read_durable_file(source, max_transaction_bytes);
  return bytes.has_value() && durable_write_new(destination, *bytes);
}

bool replace_from_file(const std::filesystem::path &source,
                       const std::filesystem::path &target) {
  const auto bytes = read_durable_file(source, max_transaction_bytes);
  return bytes.has_value() && durable_write_replacing(target, *bytes);
}

bool delete_target(const std::filesystem::path &target) {
  return remove_durable_file(target);
}

std::filesystem::path root_path(const ProfileStoreRoots &roots,
                                const RootKind kind) {
  return kind == RootKind::config ? roots.config_profiles
                                  : roots.data_profiles;
}

std::filesystem::path stage_path(const std::filesystem::path &target,
                                 const std::string_view transaction_name,
                                 const std::size_t index) {
  return target.parent_path() /
         (".layerc-" + std::string(transaction_name) + "-" +
          std::to_string(index) + ".stage");
}

std::string journal_json(const std::vector<JournalOperation> &operations) {
  std::string output = "{\"schemaVersion\":2,\"operations\":[";
  for (std::size_t index{}; index < operations.size(); ++index) {
    if (index != 0) output += ',';
    output += "{\"root\":\"";
    output += operations[index].root == RootKind::config ? "config" : "data";
    output += "\",\"target\":";
    const auto relative = path_to_utf8(operations[index].relative);
    if (!relative.has_value()) return {};
    core::json::append_quoted(output, *relative);
    output += operations[index].existed
                  ? ",\"existed\":true}"
                  : ",\"existed\":false}";
  }
  output += "]}\n";
  return output;
}

// Structural differences between journal layouts live here only: v1 drops
// the per-entry `root` field and pins every operation to the config root;
// v2 tags each entry with its root. The walk below is shared.
struct JournalSchema {
  double version_tag{};
  // When set, the entry `root` field is absent and every operation is
  // defaulted to this root. The unified-roots topology gate must hold.
  std::optional<RootKind> root_default;
};

// V2.4/V2.5 journals predate the config/data root split: a single-root
// `{"schemaVersion":1,"operations":[{"target","existed"}]}` layout with
// no `root` field and no progress file. They can only be rolled back where
// the single-root topology still holds (Windows: config == data == store);
// anywhere else a v1 journal is unrecognized residue for quarantine, not a
// rollback candidate. Missing progress (`published`) keeps the old
// conservative full-rollback behavior.
[[nodiscard]] bool journal_schema_topology_ok(const ProfileStoreRoots &roots,
                                              const JournalSchema schema) {
  return !schema.root_default.has_value() ||
         profile_roots_unified_targets(roots);
}

std::optional<std::vector<JournalOperation>>
parse_journal_operations(const ProfileStoreRoots &roots,
                         const std::string_view transaction_name,
                         const std::string_view source,
                         const JournalSchema schema) {
  if (!journal_schema_topology_ok(roots, schema)) return std::nullopt;
  const auto parsed = core::json::parse(source);
  const auto *value = std::get_if<core::json::Value>(&parsed);
  if (value == nullptr || !value->is_object()) return std::nullopt;
  const auto *version = value->find("schemaVersion");
  const auto *entries = value->find("operations");
  if (version == nullptr || !version->is_number() ||
      version->as_number() != schema.version_tag || entries == nullptr ||
      !entries->is_array() ||
      entries->as_array().size() > max_transaction_operations)
    return std::nullopt;

  std::vector<JournalOperation> operations;
  std::set<std::string, std::less<>> targets;
  for (const auto &entry : entries->as_array()) {
    if (!entry.is_object()) return std::nullopt;
    const auto *root = entry.find("root");
    const auto *target = entry.find("target");
    const auto *existed = entry.find("existed");
    RootKind kind = RootKind::config;
    if (!schema.root_default.has_value()) {
      if (root == nullptr || !root->is_string()) return std::nullopt;
      if (root->as_string() == "config")
        kind = RootKind::config;
      else if (root->as_string() == "data")
        kind = RootKind::data;
      else
        return std::nullopt;
    }
    if (target == nullptr || !target->is_string() || existed == nullptr ||
        !existed->is_bool())
      return std::nullopt;
    const auto relative = path_from_utf8(target->as_string());
    if (!relative.has_value() || !valid_relative(*relative)) return std::nullopt;
    const auto key_part = transaction_relative_key(*relative);
    const auto root_key = std::string(kind == RootKind::config ? "c:" : "d:");
    if (!key_part.has_value() || !targets.insert(root_key + *key_part).second)
      return std::nullopt;
    const auto root_dir = root_path(roots, kind);
    const auto absolute = root_dir / *relative;
    if (!safe_target(root_dir, *relative)) return std::nullopt;
    const auto index = operations.size();
    operations.push_back({.root = kind,
                          .relative = *relative,
                          .target = absolute,
                          .stage = stage_path(absolute, transaction_name, index),
                          .existed = existed->as_bool(),
                          .contents = std::nullopt});
  }
  return operations;
}

std::filesystem::path transaction_root(const ProfileStoreRoots &roots) {
  return roots.store / transaction_directory_name();
}

[[nodiscard]] bool is_quarantined(const std::filesystem::path &transaction) {
  return transaction.filename().string().find(".corrupt-") !=
         std::string::npos;
}

// Renames an unparseable transaction aside for operator inspection instead
// of deleting the only record of a half-published transaction.
[[nodiscard]] bool
quarantine_transaction(const std::filesystem::path &transaction) {
  for (std::uint64_t attempt{}; attempt < 1'000U; ++attempt) {
    std::error_code error;
    const auto quarantined = std::filesystem::path(
        transaction.string() + ".corrupt-" + std::to_string(attempt));
    std::filesystem::rename(transaction, quarantined, error);
    if (!error) return true;
  }
  return false;
}

std::optional<std::size_t>
read_published_count(const std::filesystem::path &transaction,
                     const std::size_t operation_count) {
  const auto source = read_durable_text(transaction / "published", 32U);
  if (!source.has_value() || source->empty()) return std::nullopt;
  std::size_t value{};
  const auto [end, error] =
      std::from_chars(source->data(), source->data() + source->size(), value);
  const auto finish = source->data() + source->size();
  if (error != std::errc{} ||
      (end != finish && !(end + 1 == finish && *end == '\n')) ||
      value > operation_count)
    return std::nullopt;
  return value;
}

bool write_published_count(const std::filesystem::path &transaction,
                           const std::size_t count) {
  return durable_write_replacing(transaction / "published",
                                 std::to_string(count) + "\n");
}

bool rollback(const std::filesystem::path &transaction,
              const std::vector<JournalOperation> &operations,
              const std::size_t published_count) {
  if (published_count > operations.size()) return false;
  for (std::size_t offset = published_count; offset != 0; --offset) {
    const auto index = offset - 1U;
    if (operations[index].existed) {
      if (!replace_from_file(transaction / "backup" /
                                 (std::to_string(index) + ".bin"),
                             operations[index].target))
        return false;
    } else if (!delete_target(operations[index].target)) {
      return false;
    }
  }
  return true;
}

void remove_stages(const std::vector<JournalOperation> &operations) {
  for (const auto &operation : operations)
    if (!operation.stage.empty())
      (void)remove_durable_file(operation.stage);
}

bool remove_transaction(const std::filesystem::path &transaction) {
  std::error_code error;
  std::filesystem::remove_all(transaction, error);
  return !error;
}

// Persist each directory and its entry in its parent, including newly created
// store ancestors. A file fsync alone does not make these names reachable.
bool flush_directory_hierarchy(std::filesystem::path directory) {
  while (!directory.empty()) {
    if (!fsync_directory_checked(directory)) return false;
    const auto parent = directory.parent_path();
    if (parent == directory) break;
    directory = parent;
  }
  return true;
}

bool flush_target_directories(const std::vector<JournalOperation> &operations) {
  for (const auto &operation : operations)
    if (!flush_directory_hierarchy(operation.target.parent_path())) return false;
  return true;
}

bool recover_locked(const ProfileStoreRoots &roots) {
  const auto transactions = transaction_root(roots);
  std::error_code error;
  if (!std::filesystem::exists(transactions, error)) return !error;
  if (error || file_guard_is_symlink(transactions) ||
      !file_guard_owned_directory(transactions))
    return false;

  bool quarantined{};
  for (std::filesystem::directory_iterator iterator(transactions, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const auto status = iterator->symlink_status(error);
    if (error || status.type() != std::filesystem::file_type::directory)
      return false;
    const auto transaction = iterator->path();
    // Quarantined evidence from an earlier pass: leave it alone so repeated
    // recoveries neither rename it again nor report failure for keeping it.
    if (is_quarantined(transaction)) {
      quarantined = true;
      continue;
    }
  const auto journal = read_durable_text(transaction / "journal.json",
                                         max_journal_bytes);
    if (!journal.has_value()) {
      // No journal: crashed before anything was published, so there is
      // nothing to roll back and no evidence worth keeping.
      if (!remove_transaction(transaction)) return false;
      continue;
    }
    const auto name = transaction.filename().string();
    constexpr JournalSchema schema_v2{2.0, std::nullopt};
    constexpr JournalSchema schema_v1{1.0, RootKind::config};
    auto operations =
        parse_journal_operations(roots, name, *journal, schema_v2);
    if (!operations.has_value())
      operations = parse_journal_operations(roots, name, *journal, schema_v1);
    if (!operations.has_value()) {
      // Genuinely unparseable (and not a v1 upgrade journal): quarantine for
      // operator inspection instead of deleting the only record of a
      // half-published transaction. Validation failures inside
      // parse_journal_operations (e.g. a target whose ownership changed since
      // the crash) land here too, which is exactly when the evidence matters
      // most.
      if (!quarantine_transaction(transaction)) return false;
      quarantined = true;
      continue;
    }

    const auto committed = file_guard_regular_file_exists(transaction / "committed");
    if (!committed.has_value()) return false;
    if (!*committed) {
      const auto published =
          read_published_count(transaction, operations->size());
      const auto count = published.value_or(operations->size());
      if (!rollback(transaction, *operations, count)) return false;
    }
    // A previous owner may have published a commit marker without confirming
    // its directory flush. Never reap evidence while durability is uncertain.
    if (!flush_target_directories(*operations) ||
        !flush_directory_hierarchy(transaction)) return false;
    remove_stages(*operations);
    if (!remove_transaction(transaction)) return false;
  }
  if (error) return false;
  // Best-effort: quarantined evidence intentionally keeps the directory
  // non-empty, and that retention must not fail recovery.
  std::error_code remove_error;
  std::filesystem::remove(transactions, remove_error);
  if (remove_error && !quarantined) return false;
  return true;
}

std::optional<RootKind> classify_target(const ProfileStoreRoots &roots,
                                        const std::filesystem::path &target,
                                        std::filesystem::path &relative) {
  const auto config = relative_target(roots.config_profiles, target);
  const auto data = relative_target(roots.data_profiles, target);
  if (profile_roots_unified_targets(roots) && config.has_value()) {
    relative = *config;
    return RootKind::config;
  }
  if (config.has_value() == data.has_value()) return std::nullopt;
  if (config.has_value()) {
    relative = *config;
    return RootKind::config;
  }
  relative = *data;
  return RootKind::data;
}

std::string transaction_name() {
  const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const auto process = file_guard_process_id();
  return std::to_string(process) + "-" + std::to_string(ticks) + "-" +
         std::to_string(
             transaction_sequence.fetch_add(1, std::memory_order_relaxed));
}

// Phase 1 of commit: classify mutations against the store roots, enforce
// uniqueness and the size budget, order data before config, and record
// target existence. Pure planning — nothing is written.
[[nodiscard]] std::optional<std::vector<JournalOperation>>
prepare_ops(const ProfileStoreRoots &roots,
            const std::span<const ProfileFileMutation> &mutations,
            ProfileWriteError &failure) {
  std::vector<JournalOperation> operations;
  std::set<std::string, std::less<>> unique;
  std::size_t total_bytes{};
  operations.reserve(mutations.size());
  for (const auto &mutation : mutations) {
    std::filesystem::path relative;
    const auto kind = classify_target(roots, mutation.target, relative);
    const auto key_part = transaction_relative_key(relative);
    if (!kind.has_value()) {
      failure = ProfileWriteError::scope_rejected;
      return std::nullopt;
    }
    if (!key_part.has_value() ||
        (mutation.must_be_absent && !mutation.contents.has_value()))
      return std::nullopt;
    if (!safe_target(root_path(roots, *kind), relative)) {
      failure = ProfileWriteError::scope_rejected;
      return std::nullopt;
    }
    const auto key = std::string(*kind == RootKind::config ? "c:" : "d:") +
                     *key_part;
    if (!unique.insert(key).second) return std::nullopt;
    if (mutation.contents.has_value()) {
      if (mutation.contents->size() > max_transaction_bytes - total_bytes)
        return std::nullopt;
      total_bytes += mutation.contents->size();
    }
    operations.push_back({.root = *kind,
                          .relative = relative,
                          .target = mutation.target.lexically_normal(),
                          .stage = {},
                          .existed = false,
                          .must_be_absent = mutation.must_be_absent,
                          .contents = mutation.contents});
  }
  std::stable_sort(operations.begin(), operations.end(),
                   [](const JournalOperation &left,
                      const JournalOperation &right) {
                     return left.root == RootKind::data &&
                            right.root == RootKind::config;
                   });

  for (auto &operation : operations) {
    const auto existed = file_guard_regular_file_exists(operation.target);
    if (!existed.has_value()) {
      failure = ProfileWriteError::io_failed;
      return std::nullopt;
    }
    if (operation.must_be_absent && *existed) {
      failure = ProfileWriteError::target_exists;
      return std::nullopt;
    }
    operation.existed = *existed;
  }
  return operations;
}

// Phase 2 of commit: create the transaction directory, stage contents and
// rollback copies, write the journal and progress counter, and establish the
// write-ahead durability barriers. Failure removes staged residue — nothing
// is visible yet.
[[nodiscard]] bool write_journal_and_barriers(
    const std::filesystem::path &transaction, const std::string_view name,
    std::vector<JournalOperation> &operations,
    ProfileWriteError &failure) {
  if (!file_guard_ensure_directory(transaction) ||
      !file_guard_ensure_directory(transaction / "backup")) {
    (void)remove_transaction(transaction);
    return false;
  }

  bool prepared = true;
  for (std::size_t index{}; index < operations.size(); ++index) {
    auto &operation = operations[index];
    operation.stage = stage_path(operation.target, name, index);
    if (operation.existed &&
        !copy_durable(operation.target,
                      transaction / "backup" /
                          (std::to_string(index) + ".bin"))) {
      prepared = false;
      break;
    }
    if (operation.contents.has_value() &&
        !durable_write_new(operation.stage, *operation.contents)) {
      prepared = false;
      break;
    }
  }
  const auto journal = journal_json(operations);
  if (!prepared || journal.size() > max_journal_bytes ||
      !durable_write_new(transaction / "journal.json", journal) ||
      !durable_write_new(transaction / "published", "0\n")) {
    remove_stages(operations);
    (void)remove_transaction(transaction);
    return false;
  }
  // Checked write-ahead barrier: the journal hierarchy must be durable
  // before the first target publication, otherwise recovery could rely on
  // directory entries whose durability was never established. Nothing is
  // visible yet, so failing here is safe (no rollback of published bytes).
  if (!flush_directory_hierarchy(transaction / "backup") ||
      !flush_target_directories(operations)) {
    failure = ProfileWriteError::durability_unconfirmed;
    remove_stages(operations);
    (void)remove_transaction(transaction);
    return false;
  }
  return true;
}

struct PublishProgress {
  std::size_t published{};
  std::size_t rollback_count{};
  bool unconfirmed{};
};

// Phase 3 of commit: publish each operation under the write-ahead progress
// file. Returns nullopt only when fail_after_publish (test-only fault
// injection, see profile_transaction_internal.hpp) simulated process loss
// mid-publish — the caller must leave residue for recover_locked.
[[nodiscard]] std::optional<PublishProgress> publish_ops(
    const std::filesystem::path &transaction,
    const std::vector<JournalOperation> &operations,
    const std::optional<std::size_t> &fail_after_publish) {
  PublishProgress progress;
  for (std::size_t index{}; index < operations.size(); ++index) {
    if (!write_published_count(transaction, index + 1U)) break;
    bool succeeded{};
    if (operations[index].contents.has_value()) {
      const auto outcome = publish_durable_file_checked(
          operations[index].stage, operations[index].target);
      progress.unconfirmed |=
          outcome == DurablePublishOutcome::published_unconfirmed;
      succeeded = outcome != DurablePublishOutcome::failed;
      // A failed rename publishes nothing on both platform implementations,
      // so the still-locked target must not be included in rollback.
      progress.rollback_count = succeeded ? index + 1U : index;
    } else {
      // A delete can remove the target before its durability barrier fails.
      // Include it conservatively in rollback even when delete_target fails.
      succeeded = delete_target(operations[index].target);
      progress.rollback_count = index + 1U;
    }
    if (!succeeded) break;
    progress.published = index + 1U;
    if (fail_after_publish == progress.published) return std::nullopt;
  }
  return progress;
}

// Phase 4 of commit: mark committed and reap residue, or roll back an
// incomplete publish. Leaves recovery evidence whenever durability is
// unconfirmed. Rollback of an already-open recovery pass stays in
// recover_locked.
[[nodiscard]] ProfileWriteResult finish_or_recover(
    const std::filesystem::path &transaction,
    const std::filesystem::path &transactions,
    const std::vector<JournalOperation> &operations,
    const PublishProgress &progress) {
  if (progress.published != operations.size()) {
    if (progress.unconfirmed) return {ProfileWriteError::durability_unconfirmed};
    if (rollback(transaction, operations, progress.rollback_count)) {
      remove_stages(operations);
      (void)remove_transaction(transaction);
    }
    return {ProfileWriteError::io_failed};
  }
  // Retry unconfirmed publications before acknowledging the bundle. If the
  // barrier still fails, retain the uncommitted journal and rollback copies
  // for recovery under the next writer's ownership.
  if (progress.unconfirmed && !flush_target_directories(operations))
    return {ProfileWriteError::durability_unconfirmed};
  if (!durable_write_new(transaction / "committed", "committed\n")) {
    (void)remove_durable_file(transaction / "committed");
    if (rollback(transaction, operations, progress.published)) {
      remove_stages(operations);
      (void)remove_transaction(transaction);
    }
    return {ProfileWriteError::io_failed};
  }
  if (!fsync_parent_checked(transaction / "committed"))
    return {ProfileWriteError::durability_unconfirmed};
  // Cleanup is best-effort: the transaction is fully published and marked
  // committed above, so residue-removal failures (antivirus/sharing races)
  // must not flip a durable commit into a reported failure. Leftover residue
  // is exactly what recover_profile_transactions reaps on the next run.
  remove_stages(operations);
  (void)remove_transaction(transaction);
  std::error_code error;
  std::filesystem::remove(transactions, error);
  return {};
}

} // namespace

// Internal bridge for test-only fault injection (see
// profile_transaction_internal.hpp): the production wrapper below always
// passes std::nullopt.
ProfileWriteResult commit_profile_transaction_impl(
    const ProfileStoreRoots &roots,
    const std::span<const ProfileFileMutation> &mutations,
    const std::optional<std::size_t> &fail_after_publish) {
  if (mutations.empty() || mutations.size() > max_transaction_operations)
    return {ProfileWriteError::invalid_input};
  if (!ensure_guard_transaction_roots(roots))
    return {ProfileWriteError::scope_rejected};
  TransactionFileLock lock(roots);
  if (!lock.locked()) return {ProfileWriteError::lock_busy};
  if (!recover_locked(roots)) return {ProfileWriteError::recovery_blocked};

  ProfileWriteError failure = ProfileWriteError::invalid_input;
  auto operations = prepare_ops(roots, mutations, failure);
  if (!operations.has_value()) return {failure};

  const auto transactions = transaction_root(roots);
  if (!file_guard_ensure_directory(transactions))
    return {ProfileWriteError::io_failed};
  const auto name = transaction_name();
  const auto transaction = transactions / name;
  failure = ProfileWriteError::io_failed;
  if (!write_journal_and_barriers(transaction, name, *operations, failure))
    return {failure};

  const auto progress =
      publish_ops(transaction, *operations, fail_after_publish);
  // Fault injection simulated process loss mid-publish: leave the journal
  // and partial publications for recover_locked under the next writer.
  if (!progress.has_value()) return {ProfileWriteError::io_failed};
  return finish_or_recover(transaction, transactions, *operations, *progress);
}

std::optional<ProfileStoreRoots>
profile_transaction_scope(const std::filesystem::path &profile_directory) {
  return guard_transaction_scope(profile_directory);
}

ProfileWriteResult recover_profile_transactions(const ProfileStoreRoots &roots) {
  if (!ensure_guard_transaction_roots(roots))
    return {ProfileWriteError::scope_rejected};
  TransactionFileLock lock(roots);
  if (!lock.locked()) return {ProfileWriteError::lock_busy};
  if (!recover_locked(roots)) return {ProfileWriteError::recovery_blocked};
  return {};
}

ProfileWriteResult commit_profile_transaction(
    const ProfileStoreRoots &roots,
    const std::span<const ProfileFileMutation> mutations) {
  return commit_profile_transaction_impl(roots, mutations, std::nullopt);
}
} // namespace layerc::platform
