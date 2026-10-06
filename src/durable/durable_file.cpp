// Windows side of layerc/durable/durable_file.hpp.
#include "layerc/durable/durable_file.hpp"
#include "io_chunk_size.hpp"
#include "layerc/platform/win32_handle.hpp"

#include <utility>

#include <windows.h>

namespace layerc::platform {
namespace {

// Publication only ever replaces a plain file. MoveFileExW would replace a
// symlink or junction itself, so refuse reparse points (read_durable_file
// refuses them too) and directories; an absent destination is fine.
bool publishable_destination(const std::filesystem::path &destination) {
  const DWORD attributes = GetFileAttributesW(destination.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
  }
  return (attributes &
          (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

} // namespace

std::uint64_t current_process_id() noexcept {
  return static_cast<std::uint64_t>(GetCurrentProcessId());
}

FileReadResult read_durable_file(const std::filesystem::path &path,
                                 const std::size_t maximum_bytes) {
  const OwnedHandle owned_file(CreateFileW(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  const HANDLE file = owned_file.get();
  if (!owned_file.valid()) {
    const auto error = GetLastError();
    return {.status = error == ERROR_FILE_NOT_FOUND ||
                               error == ERROR_PATH_NOT_FOUND
                           ? FileReadStatus::missing
                           : FileReadStatus::io_error,
            .bytes = {}};
  }

  LARGE_INTEGER size{};
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart < 0 ||
      GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes,
                                   sizeof(attributes)) == FALSE ||
      (attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
    return {.status = FileReadStatus::io_error, .bytes = {}};
  if (static_cast<unsigned long long>(size.QuadPart) > maximum_bytes)
    return {.status = FileReadStatus::too_large, .bytes = {}};

  std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset{};
  while (offset < bytes.size()) {
    DWORD read{};
    if (ReadFile(file, bytes.data() + offset, io_chunk_size(bytes.size() - offset),
                 &read, nullptr) == FALSE ||
        read == 0)
      return {.status = FileReadStatus::io_error, .bytes = {}};
    offset += read;
  }
  return {.status = FileReadStatus::ok, .bytes = std::move(bytes)};
}

bool create_durable_file(const std::filesystem::path &path,
                         const std::span<const std::byte> contents) {
  OwnedHandle owned_file(
      CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
  const HANDLE file = owned_file.get();
  if (!owned_file.valid()) return false;

  std::size_t offset{};
  bool success = true;
  while (offset < contents.size()) {
    DWORD written{};
    if (WriteFile(file, contents.data() + offset,
                  io_chunk_size(contents.size() - offset), &written,
                  nullptr) == FALSE ||
        written == 0) {
      success = false;
      break;
    }
    offset += written;
  }
  success = success && FlushFileBuffers(file) != FALSE;
  // Close before deleting a partial file.
  owned_file.reset();
  if (!success) (void)DeleteFileW(path.c_str());
  return success;
}

bool flush_durable_file(const std::filesystem::path &path) {
  const OwnedHandle file(CreateFileW(
      path.c_str(), GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  return file.valid() &&
         GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo,
                                     &attributes, sizeof(attributes)) != FALSE &&
         (attributes.FileAttributes &
          (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
         FlushFileBuffers(file.get()) != FALSE;
}

DurablePublishOutcome publish_durable_directory_checked(
    const std::filesystem::path &stage,
    const std::filesystem::path &destination) {
  const DWORD attributes = GetFileAttributesW(stage.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES)
    return DurablePublishOutcome::failed;
  const auto error = GetLastError();
  if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
    return DurablePublishOutcome::failed;
  // Match the file-publication NTFS contract, without replacing a directory.
  return MoveFileExW(stage.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)
                 != FALSE
             ? DurablePublishOutcome::published_durable
             : DurablePublishOutcome::failed;
}

DurablePublishOutcome publish_durable_file_checked(
    const std::filesystem::path &stage,
    const std::filesystem::path &destination) {
  // MOVEFILE_WRITE_THROUGH commits the rename durably on NTFS; there is no
  // separate parent-fsync step to fail, so a successful move is durably
  // published and a failed move published nothing.
  if (!publishable_destination(destination))
    return DurablePublishOutcome::failed;
  return MoveFileExW(stage.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE
             ? DurablePublishOutcome::published_durable
             : DurablePublishOutcome::failed;
}

bool fsync_directory_checked(const std::filesystem::path &directory) noexcept {
  // NTFS directory durability rides on the WRITE_THROUGH rename above; the
  // checked barrier is a no-op that reports success so the transaction's
  // write-ahead protocol has a single cross-platform shape.
  return !directory.empty();
}

bool remove_durable_file(const std::filesystem::path &path) {
  return DeleteFileW(path.c_str()) != FALSE ||
         GetLastError() == ERROR_FILE_NOT_FOUND;
}

// Share mode 0 is the lock: no other handle can open the file while this one
// is alive, and the kernel releases it when the process dies.
ExclusiveFileLock::ExclusiveFileLock(const std::filesystem::path &path) {
  if (!publishable_destination(path)) return;
  OwnedHandle file(CreateFileW(
      path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  BY_HANDLE_FILE_INFORMATION info{};
  if (!file.valid() || GetFileInformationByHandle(file.get(), &info) == FALSE ||
      (info.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
    return;
  native_ = reinterpret_cast<std::intptr_t>(file.release());
}

ExclusiveFileLock::~ExclusiveFileLock() {
  if (locked()) CloseHandle(reinterpret_cast<HANDLE>(native_));
}

} // namespace layerc::platform
