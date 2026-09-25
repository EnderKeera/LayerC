#include "layerc/platform/durable_file.hpp"
#include "../durable_file_internal.hpp"

#include <cerrno>
#include <cstdint>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace layerc::platform {
namespace {

// Publication only ever replaces a regular file. rename(2) would silently
// replace a symlink itself (and fails on a directory anyway); refuse both so
// behaviour matches read_durable_file's O_NOFOLLOW and the Windows
// reparse-point refusal. An absent destination is fine.
bool publishable_destination(const std::filesystem::path &destination) {
  struct stat status {};
  if (::lstat(destination.c_str(), &status) != 0)
    return errno == ENOENT || errno == ENOTDIR;
  return !S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode);
}

} // namespace

std::uint64_t detail::durable_process_id() noexcept {
  return static_cast<std::uint64_t>(::getpid());
}

FileReadResult read_durable_file(const std::filesystem::path &path,
                                 const std::size_t maximum_bytes) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return {.status = errno == ENOENT || errno == ENOTDIR
                           ? FileReadStatus::missing
                           : FileReadStatus::io_error,
            .bytes = {}};
  }
  struct stat status {};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0) {
    ::close(descriptor);
    return {.status = FileReadStatus::io_error, .bytes = {}};
  }
  if (static_cast<std::uintmax_t>(status.st_size) > maximum_bytes) {
    ::close(descriptor);
    return {.status = FileReadStatus::too_large, .bytes = {}};
  }

  std::vector<std::byte> bytes(static_cast<std::size_t>(status.st_size));
  std::size_t offset{};
  while (offset < bytes.size()) {
    const auto count =
        ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      ::close(descriptor);
      return {.status = FileReadStatus::io_error, .bytes = {}};
    }
    offset += static_cast<std::size_t>(count);
  }
  ::close(descriptor);
  return {.status = FileReadStatus::ok, .bytes = std::move(bytes)};
}

bool create_durable_file(const std::filesystem::path &path,
                         const std::span<const std::byte> contents) {
  const int descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
             S_IRUSR | S_IWUSR);
  if (descriptor < 0) return false;

  std::size_t offset{};
  bool success = true;
  while (offset < contents.size()) {
    const auto written = ::write(descriptor, contents.data() + offset,
                                 contents.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      success = false;
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
  success = success && ::fchmod(descriptor, S_IRUSR | S_IWUSR) == 0 &&
            ::fsync(descriptor) == 0;
  ::close(descriptor);
  if (!success) (void)::unlink(path.c_str());
  return success;
}

DurablePublishOutcome publish_durable_file_checked(
    const std::filesystem::path &stage,
    const std::filesystem::path &destination) {
  if (!publishable_destination(destination) ||
      ::rename(stage.c_str(), destination.c_str()) != 0)
    return DurablePublishOutcome::failed;
  // Publication has committed once rename succeeds. A directory fsync failure
  // may weaken crash durability, but reporting failure would invite rollback of
  // already-visible bytes — hence the three-way outcome: the owner must
  // preserve evidence and surface a fault instead of rolling back.
  return fsync_parent_checked(destination)
             ? DurablePublishOutcome::published_durable
             : DurablePublishOutcome::published_unconfirmed;
}

bool fsync_directory_checked(const std::filesystem::path &directory) noexcept {
  if (directory.empty()) return false;
  const int descriptor =
      ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return false;
  const bool ok = ::fsync(descriptor) == 0;
  ::close(descriptor);
  return ok;
}

bool remove_durable_file(const std::filesystem::path &path) {
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) return false;
  // The removal is only durable once the parent entry is flushed. A false
  // result here means the file may already be gone; callers that need the
  // barrier (profile transaction deletes) treat it as a failed delete.
  return fsync_parent_checked(path);
}

} // namespace layerc::platform
