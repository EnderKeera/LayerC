#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
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

// Expected-like result: `failure` is the typed error, ok() is success.
struct ProfileWriteResult {
  std::optional<ProfileWriteError> failure;

  [[nodiscard]] bool ok() const noexcept { return !failure.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
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
// while split stores accept only the configured profile directory. Each
// platform's profile_file_guard TU defines the rule.
[[nodiscard]] std::optional<ProfileStoreRoots>
profile_transaction_scope(const std::filesystem::path &profile_directory);

// Where a transaction runs: already-resolved roots, or a profile directory
// resolved through profile_transaction_scope (refusal is scope_rejected).
class ProfileTransactionScope {
public:
  ProfileTransactionScope(const ProfileStoreRoots &roots) : target_(&roots) {}
  ProfileTransactionScope(std::filesystem::path profile_directory)
      : target_(std::move(profile_directory)) {}

  [[nodiscard]] std::optional<ProfileStoreRoots> resolve() const;

private:
  std::variant<const ProfileStoreRoots *, std::filesystem::path> target_;
};

// Store-dependent planning runs only after acquiring writer ownership and
// recovering prior journals. The planner returns value-owned mutations; the
// engine stages and commits them without releasing or reacquiring ownership.
// An empty successful plan is a no-op. Do not call another writer in a planner.
using ProfileMutationPlanner =
    std::function<ProfileWriteResult(std::vector<ProfileFileMutation> &)>;

// A planner, or a fixed mutation list (a constant planner; empty is invalid).
class ProfileMutationPlan {
public:
  template <typename Planner>
    requires std::is_invocable_r_v<ProfileWriteResult, Planner &,
                                   std::vector<ProfileFileMutation> &>
  ProfileMutationPlan(Planner planner) : planner_(std::move(planner)) {}
  template <typename Mutations>
    requires std::is_convertible_v<const Mutations &,
                                   std::span<const ProfileFileMutation>>
  ProfileMutationPlan(const Mutations &mutations)
      : planner_(constant(mutations)) {}

  [[nodiscard]] ProfileWriteResult
  operator()(std::vector<ProfileFileMutation> &mutations) const {
    if (!planner_) return {ProfileWriteError::invalid_input};
    return planner_(mutations);
  }

private:
  static ProfileMutationPlanner
  constant(const std::span<const ProfileFileMutation> mutations) {
    return [mutations](std::vector<ProfileFileMutation> &out)
               -> ProfileWriteResult {
      if (mutations.empty()) return {ProfileWriteError::invalid_input};
      out.assign(mutations.begin(), mutations.end());
      return {};
    };
  }

  ProfileMutationPlanner planner_;
};

// The one locked entry point: resolve the scope, acquire writer ownership,
// recover prior journals, plan, then publish behind one durable recovery
// journal. No profile reload should be signaled until it succeeds.
[[nodiscard]] ProfileWriteResult
commit_profile_transaction(const ProfileTransactionScope &scope,
                           const ProfileMutationPlan &plan);

// Rolls back uncommitted journals and removes completed transaction residue
// (commit_profile_transaction with an empty plan). Safe to call at every
// LayerD and LayerE startup.
[[nodiscard]] ProfileWriteResult
recover_profile_transactions(const ProfileTransactionScope &scope);

} // namespace layerc::platform
