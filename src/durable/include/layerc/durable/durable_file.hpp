#pragma once

// Dependency-free durable file primitives shared by layerc_platform and the
// launcher update engine. One TU per platform implements the OS calls; this
// header names no platform types so either side can include it.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace layerc::platform {

enum class FileReadStatus : std::uint8_t {
  ok,
  missing,
  too_large,
  io_error,
};

struct FileReadResult {
  FileReadStatus status{FileReadStatus::io_error};
  std::vector<std::byte> bytes;

  [[nodiscard]] bool has_value() const noexcept {
    return status == FileReadStatus::ok;
  }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] const std::vector<std::byte> &operator*() const noexcept {
    return bytes;
  }
  [[nodiscard]] std::vector<std::byte> &operator*() noexcept { return bytes; }
  [[nodiscard]] const std::vector<std::byte> *operator->() const noexcept {
    return &bytes;
  }
  [[nodiscard]] std::vector<std::byte> *operator->() noexcept { return &bytes; }
};

// Current process id: keeps concurrent writers' stage and journal names apart.
[[nodiscard]] std::uint64_t current_process_id() noexcept;

[[nodiscard]] FileReadResult
read_durable_file(const std::filesystem::path &path, std::size_t maximum_bytes);

// Creates a new owner-only file and fails if the path already exists.
[[nodiscard]] bool create_durable_file(
    const std::filesystem::path &path, std::span<const std::byte> contents);

// Flushes an existing plain file, including bytes produced by copy_file.
// Directories, symlinks and reparse points are refused.
[[nodiscard]] bool flush_durable_file(const std::filesystem::path &path);

// Publication outcome with crash-durability distinguished at the owning
// boundary: not-published (rename failed, nothing visible), published but
// durability-unconfirmed (rename succeeded, parent fsync failed — the bytes
// are visible and must NOT be rolled back), or durably published (rename
// plus parent fsync both succeeded).
enum class DurablePublishOutcome : std::uint8_t {
  failed,
  published_unconfirmed,
  published_durable,
};

// Refuses (failed) to replace an existing directory, symlink, or reparse
// point: publication only ever replaces a plain file.
[[nodiscard]] DurablePublishOutcome publish_durable_file_checked(
    const std::filesystem::path &stage,
    const std::filesystem::path &destination);

// Moves a plain directory to an absent destination on the same filesystem.
// Checks both parent-entry barriers; an unconfirmed move stays visible and
// must not be undone or pruned by the caller.
[[nodiscard]] DurablePublishOutcome publish_durable_directory_checked(
    const std::filesystem::path &stage,
    const std::filesystem::path &destination);

// Write and publish through an adjacent unique stage. The caller owns parent
// preparation; an unconfirmed publication remains visible and is never deleted.
[[nodiscard]] DurablePublishOutcome write_durable_file(
    const std::filesystem::path &destination, std::span<const std::byte> contents);

// Checked parent-directory fsync used as a write-ahead durability barrier
// for journal hierarchies before the first target publication.
[[nodiscard]] bool fsync_directory_checked(
    const std::filesystem::path &directory) noexcept;
[[nodiscard]] bool fsync_parent_checked(
    const std::filesystem::path &path) noexcept;
// Persists a directory and its entry in each ancestor (new hierarchies too).
// On Windows, directory publication uses WRITE_THROUGH moves instead of fsync.
[[nodiscard]] bool flush_directory_hierarchy(std::filesystem::path directory);

// Removes path (absent counts as removed) behind the same checked parent
// barrier as publication; false after a failed barrier means the file may
// already be gone.
[[nodiscard]] bool remove_durable_file(const std::filesystem::path &path);
[[nodiscard]] std::filesystem::path
unique_durable_stage_path(const std::filesystem::path &destination);

// Non-blocking exclusive writer lock on a plain owner-only lock file, held
// until destruction. The file is created when absent; a directory, symlink,
// reparse point, foreign-owned file (POSIX), or a file another writer already
// holds leaves the lock unacquired. Windows locks through an exclusive share
// mode, POSIX through flock(LOCK_EX | LOCK_NB).
class ExclusiveFileLock {
public:
  explicit ExclusiveFileLock(const std::filesystem::path &path);
  ~ExclusiveFileLock();
  ExclusiveFileLock(const ExclusiveFileLock &) = delete;
  ExclusiveFileLock &operator=(const ExclusiveFileLock &) = delete;

  [[nodiscard]] bool locked() const noexcept { return native_ != -1; }

private:
  // HANDLE value on Windows (INVALID_HANDLE_VALUE is -1), descriptor on POSIX.
  std::intptr_t native_{-1};
};

} // namespace layerc::platform
