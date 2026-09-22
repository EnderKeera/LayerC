#pragma once

#include "layerc/platform/profile_transaction.hpp"
#include "layerc/platform/profile_store.hpp"

#include <cstddef>
#include <span>

namespace layerc::platform::linux_testing {

// Simulates process loss immediately after the one-based publish operation,
// intentionally leaving recovery residue behind.
[[nodiscard]] bool commit_profile_transaction_fail_after_publish(
    const ProfileStoreRoots &roots,
    std::span<const ProfileFileMutation> mutations,
    std::size_t publish_count);

} // namespace layerc::platform::linux_testing
