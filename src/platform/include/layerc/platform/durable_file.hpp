#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
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

[[nodiscard]] inline std::optional<std::string>
read_durable_text(const std::filesystem::path &path,
                  const std::size_t maximum_bytes) {
  auto result = read_durable_file(path, maximum_bytes);
  if (!result.has_value()) return std::nullopt;
  if (result->empty()) return std::string{};
  return std::string(reinterpret_cast<const char *>(result->data()),
                     result->size());
}

// Creates a new owner-only file and fails if the path already exists.
[[nodiscard]] bool create_durable_file(
    const std::filesystem::path &path, std::span<const std::byte> contents);

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

// Removes path (absent counts as removed) behind the same checked parent
// barrier as publication; false after a failed barrier means the file may
// already be gone.
[[nodiscard]] bool remove_durable_file(const std::filesystem::path &path);
[[nodiscard]] std::filesystem::path
unique_durable_stage_path(const std::filesystem::path &destination);

} // namespace layerc::platform
