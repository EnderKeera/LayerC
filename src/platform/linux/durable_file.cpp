#include "layerc/platform/durable_file.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace layerc::platform {
namespace {

std::atomic_uint64_t stage_sequence{};

void flush_parent_best_effort(const std::filesystem::path &path) {
  const auto parent = path.parent_path();
  const int descriptor =
      ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return;
  (void)::fsync(descriptor);
  ::close(descriptor);
}

} // namespace

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

bool publish_durable_file(const std::filesystem::path &stage,
                          const std::filesystem::path &destination) {
  return publish_durable_file_checked(stage, destination) !=
         DurablePublishOutcome::failed;
}

DurablePublishOutcome publish_durable_file_checked(
    const std::filesystem::path &stage,
    const std::filesystem::path &destination) {
  if (::rename(stage.c_str(), destination.c_str()) != 0)
    return DurablePublishOutcome::failed;
  // Publication has committed once rename succeeds. A directory fsync failure
  // may weaken crash durability, but reporting failure would invite rollback of
  // already-visible bytes — hence the three-way outcome: the owner must
  // preserve evidence and surface a fault instead of rolling back.
  return fsync_parent_checked(destination)
             ? DurablePublishOutcome::published_durable
             : DurablePublishOutcome::published_unconfirmed;
}

bool fsync_parent_checked(const std::filesystem::path &path) noexcept {
  return fsync_directory_checked(path.parent_path());
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
  flush_parent_best_effort(path);
  return true;
}

std::filesystem::path
unique_durable_stage_path(const std::filesystem::path &destination) {
  auto stage = destination;
  stage += "." + std::to_string(::getpid()) + "." +
           std::to_string(
               stage_sequence.fetch_add(1, std::memory_order_relaxed)) +
           ".tmp";
  return stage;
}

} // namespace layerc::platform
