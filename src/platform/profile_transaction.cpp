#include "layerc/platform/profile_transaction.hpp"

#include "layerc/core/json.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <system_error>

#include <windows.h>

namespace layerc::platform {
namespace {

constexpr std::size_t max_transaction_bytes = 128U * 1024U * 1024U;
constexpr std::size_t max_transaction_operations = 512U;
constexpr std::wstring_view transaction_directory_name = L"_transactions";
std::atomic_uint64_t transaction_sequence{};

struct JournalOperation {
  std::filesystem::path target;
  bool existed{};
};

bool valid_relative(const std::filesystem::path &relative) {
  if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
      relative.has_root_directory())
    return false;
  bool first = true;
  for (const auto &part : relative) {
    if (part.empty() || part == L"." || part == L"..") return false;
    if (first && _wcsicmp(part.c_str(), transaction_directory_name.data()) == 0)
      return false;
    first = false;
  }
  return true;
}

std::optional<std::filesystem::path>
relative_target(const std::filesystem::path &root,
                const std::filesystem::path &target) {
  std::error_code error;
  const auto absolute_root =
      std::filesystem::absolute(root, error).lexically_normal();
  if (error) return std::nullopt;
  const auto absolute_target =
      std::filesystem::absolute(target, error).lexically_normal();
  if (error) return std::nullopt;
  const auto relative = absolute_target.lexically_relative(absolute_root);
  return valid_relative(relative) ? std::optional(relative) : std::nullopt;
}

bool durable_write(const std::filesystem::path &path,
                   const std::span<const std::byte> bytes) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error || bytes.size() >
                   static_cast<std::size_t>(std::numeric_limits<DWORD>::max()))
    return false;
  const HANDLE file = CreateFileW(
      path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written{};
  const bool ok =
      (bytes.empty() ||
       (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                  &written, nullptr) != FALSE &&
        written == bytes.size())) &&
      FlushFileBuffers(file) != FALSE;
  CloseHandle(file);
  return ok;
}

bool durable_write(const std::filesystem::path &path,
                   const std::string_view text) {
  return durable_write(
      path, std::span(reinterpret_cast<const std::byte *>(text.data()),
                      text.size()));
}

bool flush_file(const std::filesystem::path &path) {
  const HANDLE file = CreateFileW(
      path.c_str(), GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  const bool flushed = FlushFileBuffers(file) != FALSE;
  CloseHandle(file);
  return flushed;
}

std::optional<std::string> read_bounded(const std::filesystem::path &path,
                                        const std::size_t maximum) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > maximum) return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  std::string result(static_cast<std::size_t>(size), '\0');
  input.read(result.data(), static_cast<std::streamsize>(result.size()));
  return input.gcount() == static_cast<std::streamsize>(result.size())
             ? std::optional(std::move(result))
             : std::nullopt;
}

bool replace_from_file(const std::filesystem::path &source,
                       const std::filesystem::path &target) {
  std::error_code error;
  std::filesystem::create_directories(target.parent_path(), error);
  if (error) return false;
  auto temporary = target;
  temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".txn.tmp";
  if (CopyFileW(source.c_str(), temporary.c_str(), FALSE) == FALSE) return false;
  if (!flush_file(temporary)) {
    DeleteFileW(temporary.c_str());
    return false;
  }
  if (MoveFileExW(temporary.c_str(), target.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
      FALSE) {
    DeleteFileW(temporary.c_str());
    return false;
  }
  return true;
}

bool delete_target(const std::filesystem::path &target) {
  return DeleteFileW(target.c_str()) != FALSE ||
         GetLastError() == ERROR_FILE_NOT_FOUND;
}

std::string journal_json(const std::filesystem::path &root,
                         const std::vector<JournalOperation> &operations) {
  std::string output = "{\"schemaVersion\":1,\"operations\":[";
  for (std::size_t index = 0; index < operations.size(); ++index) {
    const auto relative = relative_target(root, operations[index].target);
    if (!relative.has_value()) return {};
    const auto generic = try_wide_to_utf8(relative->generic_wstring());
    if (!generic.has_value()) return {};
    if (index != 0) output += ',';
    output += "{\"target\":";
    core::json::append_quoted(output, *generic);
    output += operations[index].existed
                  ? ",\"existed\":true}"
                  : ",\"existed\":false}";
  }
  output += "]}\n";
  return output;
}

std::optional<std::vector<JournalOperation>>
parse_journal(const std::filesystem::path &root,
              const std::string_view source) {
  const auto parsed = core::json::parse(source);
  const auto *value = std::get_if<core::json::Value>(&parsed);
  if (value == nullptr || !value->is_object()) return std::nullopt;
  const auto *schema = value->find("schemaVersion");
  const auto *operations = value->find("operations");
  if (schema == nullptr || !schema->is_number() ||
      schema->as_number() != 1.0 || operations == nullptr ||
      !operations->is_array() ||
      operations->as_array().size() > max_transaction_operations)
    return std::nullopt;
  std::vector<JournalOperation> result;
  std::set<std::wstring, std::less<>> targets;
  for (const auto &entry : operations->as_array()) {
    if (!entry.is_object()) return std::nullopt;
    const auto *target = entry.find("target");
    const auto *existed = entry.find("existed");
    if (target == nullptr || !target->is_string() || existed == nullptr ||
        !existed->is_bool())
      return std::nullopt;
    const auto wide = try_utf8_to_wide(target->as_string());
    if (!wide.has_value()) return std::nullopt;
    const std::filesystem::path relative(*wide);
    const auto folded = try_invariant_lower(relative.generic_wstring());
    if (!valid_relative(relative) || !folded.has_value() ||
        !targets.insert(*folded).second)
      return std::nullopt;
    result.push_back(
        {.target = root / relative, .existed = existed->as_bool()});
  }
  return result;
}

bool rollback(const std::filesystem::path &transaction,
              const std::vector<JournalOperation> &operations) {
  for (std::size_t offset = operations.size(); offset != 0; --offset) {
    const auto index = offset - 1U;
    const auto backup =
        transaction / L"backup" / (std::to_wstring(index) + L".bin");
    if (operations[index].existed) {
      if (!replace_from_file(backup, operations[index].target)) return false;
    } else if (!delete_target(operations[index].target)) {
      return false;
    }
  }
  return true;
}

std::filesystem::path transaction_root(
    const std::filesystem::path &profile_root) {
  return profile_root / transaction_directory_name;
}

class TransactionLock {
public:
  TransactionLock() {
    mutex_ = CreateMutexW(nullptr, FALSE,
                          L"Local\\LayerC.ProfileTransaction.v1");
    if (mutex_ != nullptr) {
      const DWORD wait = WaitForSingleObject(mutex_, 30'000);
      locked_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }
  }
  ~TransactionLock() {
    if (locked_) ReleaseMutex(mutex_);
    if (mutex_ != nullptr) CloseHandle(mutex_);
  }
  [[nodiscard]] bool locked() const noexcept { return locked_; }

private:
  HANDLE mutex_{};
  bool locked_{};
};

bool recover_locked(const std::filesystem::path &profile_root) {
  const auto transactions = transaction_root(profile_root);
  std::error_code error;
  if (!std::filesystem::exists(transactions, error)) return !error;
  for (std::filesystem::directory_iterator iterator(transactions, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_directory(error) || error) return false;
    const auto directory = iterator->path();
    const auto journal_source =
        read_bounded(directory / L"journal.json", 256U * 1024U);
    if (!journal_source.has_value()) {
      std::filesystem::remove_all(directory, error);
      if (error) return false;
      continue;
    }
    const auto operations = parse_journal(profile_root, *journal_source);
    if (!operations.has_value()) return false;
    if (!std::filesystem::exists(directory / L"committed", error)) {
      if (error || !rollback(directory, *operations)) return false;
    }
    std::filesystem::remove_all(directory, error);
    if (error) return false;
  }
  if (error) return false;
  std::filesystem::remove(transactions, error);
  return !error;
}

} // namespace

bool recover_profile_transactions(
    const std::filesystem::path &profile_root) {
  TransactionLock lock;
  return lock.locked() && recover_locked(profile_root);
}

bool commit_profile_transaction(
    const std::filesystem::path &profile_root,
    const std::span<const ProfileFileMutation> mutations) {
  if (mutations.empty() || mutations.size() > max_transaction_operations)
    return false;
  TransactionLock lock;
  if (!lock.locked() || !recover_locked(profile_root)) return false;

  std::set<std::wstring, std::less<>> unique;
  std::size_t total_bytes{};
  for (const auto &mutation : mutations) {
    const auto relative = relative_target(profile_root, mutation.target);
    if (!relative.has_value()) return false;
    const auto key = try_invariant_lower(relative->generic_wstring());
    if (!key.has_value() || !unique.insert(*key).second) return false;
    if (mutation.contents.has_value()) {
      if (mutation.contents->size() > max_transaction_bytes - total_bytes)
        return false;
      total_bytes += mutation.contents->size();
    }
  }

  std::error_code error;
  std::filesystem::create_directories(transaction_root(profile_root), error);
  if (error) return false;
  const auto name =
      std::to_wstring(GetCurrentProcessId()) + L"-" +
      std::to_wstring(GetTickCount64()) + L"-" +
      std::to_wstring(
          transaction_sequence.fetch_add(1, std::memory_order_relaxed));
  const auto transaction = transaction_root(profile_root) / name;
  std::filesystem::create_directories(transaction / L"stage", error);
  std::filesystem::create_directories(transaction / L"backup", error);
  if (error) return false;

  std::vector<JournalOperation> journal;
  journal.reserve(mutations.size());
  bool prepared = true;
  for (std::size_t index = 0; index < mutations.size(); ++index) {
    const auto &mutation = mutations[index];
    const bool exists = std::filesystem::exists(mutation.target, error);
    if (error) {
      prepared = false;
      break;
    }
    journal.push_back({.target = mutation.target, .existed = exists});
    if (exists) {
      const auto backup = transaction / L"backup" /
                          (std::to_wstring(index) + L".bin");
      if (CopyFileW(mutation.target.c_str(), backup.c_str(), FALSE) == FALSE ||
          !flush_file(backup)) {
        prepared = false;
        break;
      }
    }
    if (mutation.contents.has_value() &&
        !durable_write(transaction / L"stage" /
                           (std::to_wstring(index) + L".bin"),
                       *mutation.contents)) {
      prepared = false;
      break;
    }
  }
  const auto serialized = journal_json(profile_root, journal);
  if (!prepared || journal.size() != mutations.size() || serialized.empty() ||
      !durable_write(transaction / L"journal.json", serialized)) {
    std::filesystem::remove_all(transaction, error);
    return false;
  }

  bool published = true;
  for (std::size_t index = 0; index < mutations.size(); ++index) {
    if (mutations[index].contents.has_value()) {
      published =
          replace_from_file(transaction / L"stage" /
                                (std::to_wstring(index) + L".bin"),
                            mutations[index].target);
    } else {
      published = delete_target(mutations[index].target);
    }
    if (!published) break;
  }
  if (!published) {
    if (rollback(transaction, journal))
      std::filesystem::remove_all(transaction, error);
    return false;
  }
  if (!durable_write(transaction / L"committed", "committed\n")) {
    if (rollback(transaction, journal))
      std::filesystem::remove_all(transaction, error);
    return false;
  }
  std::filesystem::remove_all(transaction, error);
  error.clear();
  std::filesystem::remove(transaction_root(profile_root), error);
  return true;
}

} // namespace layerc::platform
