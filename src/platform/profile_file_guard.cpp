// Windows side of profile_file_guard.hpp: named-mutex lock, reparse-point
// gates, case-folded keys. Unified topology (config == data == store), so
// the journal directory nests below both target roots and its name is
// reserved as a first path component.

#include "layerc/platform/profile_file_guard.hpp"

#include "layerc/platform/text.hpp"

#include <string>
#include <system_error>

#include <windows.h>

namespace layerc::platform {

std::filesystem::path transaction_directory_name() {
  return std::filesystem::path{L"_transactions"};
}

std::optional<std::string>
transaction_relative_key(const std::filesystem::path &relative) {
  const auto encoded = path_to_utf8(relative);
  if (!encoded.has_value()) return std::nullopt;
  return core::try_casefold_utf8(*encoded);
}

bool transaction_reserved_first_component(std::string_view name_utf8) {
  const auto folded = core::try_casefold_utf8(name_utf8);
  return folded.has_value() && *folded == "_transactions";
}

// Every reparse point (symbolic link, junction, mount point) can redirect a
// store path outside its root; std::filesystem reports junctions as
// file_type::junction rather than a symlink, so test the attribute directly.
bool file_guard_is_symlink(const std::filesystem::path &path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool file_guard_owned_directory(const std::filesystem::path &path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool file_guard_ensure_directory(const std::filesystem::path &path) {
  if (path.empty() || !path.is_absolute()) return false;
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    if (error || !std::filesystem::create_directory(path, error) || error)
      return false;
  }
  return file_guard_owned_directory(path);
}

bool file_guard_absent_or_regular(const std::filesystem::path &path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error) return error == std::errc::no_such_file_or_directory;
  return status.type() == std::filesystem::file_type::not_found ||
         std::filesystem::is_regular_file(status);
}

std::optional<bool>
file_guard_regular_file_exists(const std::filesystem::path &path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error)
    return error == std::errc::no_such_file_or_directory
               ? std::optional(false)
               : std::nullopt;
  if (status.type() == std::filesystem::file_type::not_found) return false;
  if (!std::filesystem::is_regular_file(status)) return std::nullopt;
  return true;
}

bool ensure_guard_transaction_roots(const ProfileStoreRoots &roots) {
  if (roots.config_profiles.empty() || roots.data_profiles.empty() ||
      roots.store.empty() || !roots.config_profiles.is_absolute() ||
      !roots.data_profiles.is_absolute() || !roots.store.is_absolute())
    return false;
  std::error_code error;
  std::filesystem::create_directories(roots.config_profiles, error);
  if (error || !file_guard_owned_directory(roots.config_profiles))
    return false;
  std::filesystem::create_directories(roots.data_profiles, error);
  if (error || !file_guard_owned_directory(roots.data_profiles)) return false;
  std::filesystem::create_directories(roots.store, error);
  return !error && file_guard_owned_directory(roots.store);
}

std::optional<ProfileStoreRoots>
guard_transaction_scope(const std::filesystem::path &profile_directory) {
  if (profile_directory.empty()) return std::nullopt;
  std::error_code error;
  const auto directory =
      std::filesystem::absolute(profile_directory, error).lexically_normal();
  if (error || !directory.is_absolute()) return std::nullopt;
  return ProfileStoreRoots{.config_profiles = directory,
                           .data_profiles = directory,
                           .store = directory};
}

struct TransactionFileLock::Impl {
  HANDLE mutex{};
  bool locked{};
};

TransactionFileLock::TransactionFileLock(const ProfileStoreRoots &roots)
    : impl_(std::make_unique<Impl>()) {
  (void)roots;
  impl_->mutex =
      CreateMutexW(nullptr, FALSE, L"Local\\LayerC.ProfileTransaction.v1");
  if (impl_->mutex == nullptr) return;
  // Timed wait: a layerE save racing a brief layerD commit (automation
  // state write, import) waits the holder out instead of failing the save.
  // Split stores keep LOCK_NB — flock has no timed wait — so a contended
  // Linux store still fails fast with a visible lock error.
  const DWORD wait = WaitForSingleObject(impl_->mutex, 30'000);
  impl_->locked = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
}

TransactionFileLock::~TransactionFileLock() {
  if (impl_ == nullptr) return;
  if (impl_->locked) (void)ReleaseMutex(impl_->mutex);
  if (impl_->mutex != nullptr) CloseHandle(impl_->mutex);
}

bool TransactionFileLock::locked() const noexcept {
  return impl_ != nullptr && impl_->locked;
}

} // namespace layerc::platform
