#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace layerc::platform {

struct ProfileStoreRoots;

enum class ProfileWriteError {
  invalid_input,
  scope_rejected,
  lock_busy,
  recovery_blocked,
  target_exists,
  io_failed,
  durability_unconfirmed,
};

[[nodiscard]] constexpr const char *profile_write_error_text(
    const ProfileWriteError error) noexcept {
  switch (error) {
  case ProfileWriteError::invalid_input: return "invalid profile data or destination";
  case ProfileWriteError::scope_rejected: return "profile store scope rejected";
  case ProfileWriteError::lock_busy: return "profile store is busy";
  case ProfileWriteError::recovery_blocked: return "profile recovery is blocked";
  case ProfileWriteError::target_exists: return "profile already exists";
  case ProfileWriteError::io_failed: return "profile store I/O failed";
  case ProfileWriteError::durability_unconfirmed: return "profile write durability is unconfirmed";
  }
  return "unknown profile write failure";
}

// Expected-like result: boolean success checks remain valid while callers
// can inspect the typed error for diagnostics.
struct ProfileWriteResult {
  std::optional<ProfileWriteError> failure;

  [[nodiscard]] operator bool() const noexcept { return !failure.has_value(); }
  [[nodiscard]] std::optional<ProfileWriteError> error() const noexcept {
    return failure;
  }
};

struct ProfileFileMutation {
  std::filesystem::path target;
  // A missing value means delete the target. Present bytes replace it.
  std::optional<std::vector<std::byte>> contents;
  // Checked while holding the transaction lock, before any staging begins.
  bool must_be_absent{};
};

// Resolves the roots governing transactions for a requested profile directory.
// The contract differs by roots topology, not by call site: unified stores
// accept an arbitrary profile directory and keep transaction state below it,
// while split stores accept only the configured profile directory. See
// guard_transaction_scope (profile_file_guard.hpp) for the per-platform rule.
[[nodiscard]] std::optional<ProfileStoreRoots>
profile_transaction_scope(const std::filesystem::path &profile_directory);

// Publishes all mutations behind one durable recovery journal. No profile
// reload should be signaled until this function succeeds.
[[nodiscard]] ProfileWriteResult commit_profile_transaction(
    const ProfileStoreRoots &roots,
    std::span<const ProfileFileMutation> mutations);

// Rolls back uncommitted journals and removes completed transaction residue.
// Safe to call at every LayerD and LayerE startup.
[[nodiscard]] ProfileWriteResult recover_profile_transactions(const ProfileStoreRoots &roots);

// Directory forms: resolve profile_transaction_scope(profile_directory) first
// and fail with scope_rejected when the directory is refused.
[[nodiscard]] ProfileWriteResult commit_profile_transaction(
    const std::filesystem::path &profile_directory,
    std::span<const ProfileFileMutation> mutations);
[[nodiscard]] ProfileWriteResult
recover_profile_transactions(const std::filesystem::path &profile_directory);

} // namespace layerc::platform
