#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace layerc::platform {

struct ProfileFileMutation {
  std::filesystem::path target;
  // A missing value means delete the target. Present bytes replace it.
  std::optional<std::vector<std::byte>> contents;
};

// Publishes all mutations behind one durable recovery journal. No profile
// reload should be signaled until this function succeeds.
[[nodiscard]] bool commit_profile_transaction(
    const std::filesystem::path &profile_root,
    std::span<const ProfileFileMutation> mutations);

// Rolls back uncommitted journals and removes completed transaction residue.
// Safe to call at every LayerD and LayerE startup.
[[nodiscard]] bool recover_profile_transactions(
    const std::filesystem::path &profile_root);

} // namespace layerc::platform
