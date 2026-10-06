#pragma once

// Platform file-guard behind the shared profile-transaction engine.
//
// profile_transaction.cpp owns journal orchestration only; every
// platform question (symlink/ownership gates, case folding, journal
// directory spelling, lock primitive, transaction scope) lives here with
// one TU per platform (win32/profile_file_guard.cpp on Windows,
// linux/profile_file_guard.cpp on Linux) and zero `#ifdef`s in the shared
// engine — the same split durable_file already established.
//
// Layering: this header may include profile_store.hpp (roots model). It
// must not include profile_transaction.hpp (the engine includes this).

#include "layerc/platform/profile_store.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::platform {

// Name of the journal directory below roots.store. Spelled per platform:
// unified Windows stores nest it below both target roots (so the guard
// also reserves it, see below); split Linux stores keep it outside both
// target roots.
[[nodiscard]] std::filesystem::path transaction_directory_name();

// Comparison key for a journal-relative path: case-folded on Windows
// (case-insensitive store), byte identity on Linux. Nullopt when the path
// is not encodable.
[[nodiscard]] std::optional<std::string>
transaction_relative_key(const std::filesystem::path &relative);

// True when a first path component collides with this platform's journal
// directory. On split stores this is always false — the journal sits
// outside both target roots, so no reservation is needed.
[[nodiscard]] bool transaction_reserved_first_component(
    std::string_view name_utf8);

// Ownership/symlink gates. Windows: directory, not a reparse point.
// Linux: lstat-owned by the euid, never a symlink.
[[nodiscard]] bool
file_guard_is_symlink(const std::filesystem::path &path);
[[nodiscard]] bool
file_guard_owned_directory(const std::filesystem::path &path);
// Creates an absolute directory when absent, then gates it: ownership on
// both platforms, owner-only mode on Linux.
[[nodiscard]] bool
file_guard_ensure_directory(const std::filesystem::path &path);
// Tail of the safe-target gate: true when the target is absent or an owned
// regular file (reparse points, directories, and foreign-uid files fail).
[[nodiscard]] bool
file_guard_absent_or_regular(const std::filesystem::path &path);
// Tri-state existence: false when absent, true when an owned regular file,
// nullopt on I/O error or when a present file is not an owned regular file.
[[nodiscard]] std::optional<bool>
file_guard_regular_file_exists(const std::filesystem::path &path);

// Platform root setup for transactions. Unified stores create and gate all
// three roots; split stores delegate to ensure_profile_store_roots.
[[nodiscard]] bool
ensure_guard_transaction_roots(const ProfileStoreRoots &roots);

// Roots governing transactions for a requested profile directory. Unified
// stores accept an arbitrary directory and keep transaction state below it;
// split stores accept only the configured profile directory.
[[nodiscard]] std::optional<ProfileStoreRoots>
profile_transaction_scope(const std::filesystem::path &profile_directory);

// One-writer lock for the transaction engine. Unified stores take a named
// OS mutex with a bounded wait; split stores take a non-blocking flock on
// the store lock file and fail fast when contended.
class TransactionFileLock {
public:
  explicit TransactionFileLock(const ProfileStoreRoots &roots);
  ~TransactionFileLock();
  TransactionFileLock(const TransactionFileLock &) = delete;
  TransactionFileLock &operator=(const TransactionFileLock &) = delete;
  TransactionFileLock(TransactionFileLock &&) = delete;
  TransactionFileLock &operator=(TransactionFileLock &&) = delete;

  [[nodiscard]] bool locked() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace layerc::platform
