#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace layerc::platform {

struct ProfileStoreRoots;

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
[[nodiscard]] bool commit_profile_transaction(
    const ProfileStoreRoots &roots,
    std::span<const ProfileFileMutation> mutations);

// Rolls back uncommitted journals and removes completed transaction residue.
// Safe to call at every LayerD and LayerE startup.
[[nodiscard]] bool recover_profile_transactions(const ProfileStoreRoots &roots);

} // namespace layerc::platform
