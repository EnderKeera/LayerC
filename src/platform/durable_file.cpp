#include "layerc/platform/durable_file.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

#include <windows.h>

namespace layerc::platform {
namespace {

std::atomic_uint64_t stage_sequence{};

} // namespace

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
    const auto remaining = bytes.size() - offset;
    const DWORD request = static_cast<DWORD>(
        (std::min)(remaining,
                   static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD read{};
    if (ReadFile(file, bytes.data() + offset, request, &read, nullptr) == FALSE ||
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
    const auto remaining = contents.size() - offset;
    const DWORD request = static_cast<DWORD>(
        (std::min)(remaining,
                   static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD written{};
    if (WriteFile(file, contents.data() + offset, request, &written, nullptr) ==
            FALSE ||
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

bool publish_durable_file(const std::filesystem::path &stage,
                          const std::filesystem::path &destination) {
  return publish_durable_file_checked(stage, destination) !=
         DurablePublishOutcome::failed;
}

DurablePublishOutcome publish_durable_file_checked(
    const std::filesystem::path &stage,
    const std::filesystem::path &destination) {
  // MOVEFILE_WRITE_THROUGH commits the rename durably on NTFS; there is no
  // separate parent-fsync step to fail, so a successful move is durably
  // published and a failed move published nothing.
  return MoveFileExW(stage.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE
             ? DurablePublishOutcome::published_durable
             : DurablePublishOutcome::failed;
}

bool fsync_parent_checked(const std::filesystem::path &path) noexcept {
  return fsync_directory_checked(path.parent_path());
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

std::filesystem::path
unique_durable_stage_path(const std::filesystem::path &destination) {
  auto stage = destination;
  stage += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(
               stage_sequence.fetch_add(1, std::memory_order_relaxed)) +
           L".tmp";
  return stage;
}

} // namespace layerc::platform
