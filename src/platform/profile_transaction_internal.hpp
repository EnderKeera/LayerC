#pragma once

// Internal bridge between the transaction engine and the fault-injection TU
// compiled into test targets only (profile_transaction_testing.cpp). Not
// part of the public platform API: production code reaches the engine
// through commit_profile_transaction (profile_transaction.hpp), which always
// passes std::nullopt. Test builds pass a one-based publish count to
// simulate process loss mid-publish and exercise recovery.
//
// Kept in its own header (rather than ifdef'd into the production TU) so
// src/platform/profile_transaction.cpp includes no test-only headers.

#include "layerc/platform/profile_transaction.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace layerc::platform {

[[nodiscard]] bool commit_profile_transaction_impl(
    const ProfileStoreRoots &roots,
    const std::span<const ProfileFileMutation> &mutations,
    const std::optional<std::size_t> &fail_after_publish);

} // namespace layerc::platform
