// Linux side of profile_file_guard.hpp: flock lock file, lstat/euid gates,
// byte-identity keys. Split topology (config vs data vs store), so the
// journal directory sits outside both target roots and no first-component
// reservation is needed.

#include "layerc/platform/profile_file_guard.hpp"
#include "layerc/platform/profile_store_linux.hpp"

#include "layerc/platform/text.hpp"

#include <cerrno>
#include <filesystem>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace layerc::platform {

std::filesystem::path transaction_directory_name() {
  return std::filesystem::path{"transactions"};
}

std::optional<std::string>
transaction_relative_key(const std::filesystem::path &relative) {
  return path_to_utf8(relative);
}

bool transaction_reserved_first_component(std::string_view name_utf8) {
  (void)name_utf8;
  return false;
}

namespace {

bool has_symlink_component(const std::filesystem::path &path) {
  std::filesystem::path current;
  for (const auto &part : path.lexically_normal()) {
    current /= part;
    struct stat status {};
    if (::lstat(current.c_str(), &status) == 0) {
      if (S_ISLNK(status.st_mode)) return true;
    } else if (errno != ENOENT) {
      return true;
    }
  }
  return false;
}

} // namespace

bool ensure_owned_directory(const std::filesystem::path &path,
                            const OwnedDirectoryPolicy policy) {
  if (path.empty() || !path.is_absolute() || has_symlink_component(path))
    return false;
  const bool require_private = policy == OwnedDirectoryPolicy::require_private;
  if (require_private) {
    if (::mkdir(path.c_str(), S_IRWXU) != 0 && errno != EEXIST) return false;
  } else {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) return false;
  }
  if (has_symlink_component(path)) return false;
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != ::geteuid())
    return false;
  if (require_private) return (status.st_mode & 0777U) == S_IRWXU;
  return ::chmod(path.c_str(), S_IRWXU) == 0;
}

bool file_guard_is_symlink(const std::filesystem::path &path) {
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0 && S_ISLNK(status.st_mode);
}

bool file_guard_owned_directory(const std::filesystem::path &path) {
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
         status.st_uid == ::geteuid();
}

bool file_guard_ensure_directory(const std::filesystem::path &path) {
  if (path.empty() || !path.is_absolute()) return false;
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    if (error || !std::filesystem::create_directory(path, error) || error)
      return false;
  }
  return !file_guard_is_symlink(path) && file_guard_owned_directory(path) &&
         ::chmod(path.c_str(), S_IRWXU) == 0;
}

bool file_guard_absent_or_regular(const std::filesystem::path &path) {
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0) return errno == ENOENT;
  return S_ISREG(status.st_mode) && status.st_uid == ::geteuid();
}

std::optional<bool>
file_guard_regular_file_exists(const std::filesystem::path &path) {
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0)
    return errno == ENOENT ? std::optional(false) : std::nullopt;
  if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid())
    return std::nullopt;
  return true;
}

bool ensure_guard_transaction_roots(const ProfileStoreRoots &roots) {
  return ensure_profile_store_roots(roots);
}

std::optional<ProfileStoreRoots>
guard_transaction_scope(const std::filesystem::path &profile_directory) {
  const auto roots = profile_store_roots();
  if (profile_directory.empty() || !profile_directory.is_absolute() ||
      profile_directory.lexically_normal() !=
          roots.config_profiles.lexically_normal())
    return std::nullopt;
  return roots;
}

std::uint64_t file_guard_process_id() noexcept {
  return static_cast<std::uint64_t>(::getpid());
}

struct TransactionFileLock::Impl {
  int descriptor_{-1};
};

TransactionFileLock::TransactionFileLock(const ProfileStoreRoots &roots)
    : impl_(std::make_unique<Impl>()) {
  const auto path = roots.store / "lock";
  impl_->descriptor_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC |
                                             O_NOFOLLOW,
                              S_IRUSR | S_IWUSR);
  if (impl_->descriptor_ < 0) return;
  struct stat status {};
  if (::fstat(impl_->descriptor_, &status) != 0 ||
      !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
      ::fchmod(impl_->descriptor_, S_IRUSR | S_IWUSR) != 0 ||
      ::flock(impl_->descriptor_, LOCK_EX | LOCK_NB) != 0) {
    ::close(impl_->descriptor_);
    impl_->descriptor_ = -1;
  }
}

TransactionFileLock::~TransactionFileLock() {
  if (impl_ == nullptr || impl_->descriptor_ < 0) return;
  (void)::flock(impl_->descriptor_, LOCK_UN);
  ::close(impl_->descriptor_);
}

bool TransactionFileLock::locked() const noexcept {
  return impl_ != nullptr && impl_->descriptor_ >= 0;
}

} // namespace layerc::platform
