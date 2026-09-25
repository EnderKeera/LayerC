// Fault-injection entry point for transaction crash-recovery tests. Compiled
// into test targets only — never into layerd/layerE — so the production
// transaction TU includes no test-only headers (see
// profile_transaction_internal.hpp).

#include "linux/profile_transaction_test.hpp"

#include "profile_transaction_internal.hpp"

namespace layerc::platform::linux_testing {

bool commit_profile_transaction_fail_after_publish(
    const ProfileStoreRoots &roots,
    const std::span<const ProfileFileMutation> mutations,
    const std::size_t publish_count) {
  if (publish_count == 0 || publish_count > mutations.size()) return false;
  return static_cast<bool>(
      commit_profile_transaction_impl(roots, mutations, publish_count));
}

} // namespace layerc::platform::linux_testing
