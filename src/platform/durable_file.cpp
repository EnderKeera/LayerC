#include "layerc/platform/durable_file.hpp"
#include "durable_file_internal.hpp"
#include "io_chunk_size.hpp"

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

std::uint64_t detail::durable_process_id() noexcept {
  return static_cast<std::uint64_t>(GetCurrentProcessId());
}

FileReadResult read_durable_file(const std::filesystem::path &path,
                                 const std::size_t maximum_bytes) {
  const HANDLE file = CreateFileW(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
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
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    CloseHandle(file);
    return {.status = FileReadStatus::io_error, .bytes = {}};
  }
  if (static_cast<unsigned long long>(size.QuadPart) > maximum_bytes) {
    CloseHandle(file);
    return {.status = FileReadStatus::too_large, .bytes = {}};
  }

  std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset{};
  while (offset < bytes.size()) {
    DWORD read{};
    if (ReadFile(file, bytes.data() + offset, io_chunk_size(bytes.size() - offset),
                 &read, nullptr) == FALSE ||
        read == 0) {
      CloseHandle(file);
      return {.status = FileReadStatus::io_error, .bytes = {}};
    }
    offset += read;
  }
  CloseHandle(file);
  return {.status = FileReadStatus::ok, .bytes = std::move(bytes)};
}

bool create_durable_file(const std::filesystem::path &path,
                         const std::span<const std::byte> contents) {
  const HANDLE file =
      CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;

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
  CloseHandle(file);
  if (!success) (void)DeleteFileW(path.c_str());
  return success;
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

} // namespace layerc::platform
