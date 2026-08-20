// LayerC Game Bar companion host (MSIX executable).
//
// The daemon owns the one-way snapshot pipe; this process consumes it. The
// host verifies the pipe server's identity (a sibling layerc/layerd.exe from
// the same install) before consuming frames so a same-user process cannot
// squat the well-known pipe and feed the widget forged snapshots (layer21-F4).
//
// NOTE: this small check mirrors layerc::platform::is_sibling_process_image
// rather than calling it, because the host intentionally does not link
// layerc_windows (it is a dependency-light MSIX package binary).
//
// Exit codes: 2 pipe unavailable, 3 oversized frame, 4 malformed frame,
// 5 connected but no usable frame (one-shot), 6 server identity not trusted.
#include "layerc/core/companion.hpp"
#include "layerc/core/frame.hpp"

#include <windows.h>
#include <winrt/Windows.ApplicationModel.AppService.h>
#include <winrt/base.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

constexpr wchar_t pipe_name[] = L"\\\\.\\pipe\\LayerC.GameBar.v1";
constexpr wchar_t app_service_name[] = L"LayerC.GameBar.v1";

bool read_exact(const HANDLE pipe, void *destination, const DWORD bytes) {
  auto *cursor = static_cast<std::byte *>(destination);
  DWORD remaining = bytes;
  while (remaining != 0) {
    DWORD received{};
    if (ReadFile(pipe, cursor, remaining, &received, nullptr) == FALSE ||
        received == 0)
      return false;
    cursor += received;
    remaining -= received;
  }
  return true;
}

bool daemon_sibling_from_pipe(const HANDLE pipe) {
  ULONG server_pid{};
  if (GetNamedPipeServerProcessId(pipe, &server_pid) == FALSE ||
      server_pid == 0)
    return false;
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server_pid);
  if (process == nullptr) return false;
  std::array<wchar_t, 32'768> own_path{};
  const DWORD own_length = GetModuleFileNameW(
      nullptr, own_path.data(), static_cast<DWORD>(own_path.size()));
  std::array<wchar_t, 32'768> server_path{};
  DWORD server_length = static_cast<DWORD>(server_path.size());
  const bool queried =
      own_length != 0 && own_length < own_path.size() &&
      QueryFullProcessImageNameW(process, 0, server_path.data(),
                                 &server_length) == TRUE &&
      server_length != 0;
  CloseHandle(process);
  if (!queried) return false;
  const std::filesystem::path expected =
      std::filesystem::path(std::wstring_view(own_path.data(), own_length))
          .parent_path() /
      L"layerd.exe";
  const std::filesystem::path actual =
      std::filesystem::path(std::wstring_view(server_path.data(), server_length));
  std::error_code error;
  return std::filesystem::equivalent(expected, actual, error) && !error;
}

unsigned next_backoff(unsigned milliseconds) {
  return milliseconds < 1'000U ? milliseconds * 2U : 2'000U;
}

int run_host() {
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  // Construct the C++/WinRT AppService type so the host is package-ready. The
  // daemon-owned named pipe remains the one-way immutable snapshot channel.
  const winrt::Windows::ApplicationModel::AppService::AppServiceConnection
      app_service{};
  (void)app_service;
  (void)app_service_name;
  const bool one_shot = GetEnvironmentVariableW(
                            L"LAYERC_GAMEBAR_ONESHOT", nullptr, 0) != 0;

  unsigned backoff_ms = 100;
  for (;;) {
    if (WaitNamedPipeW(pipe_name, 2'000) == FALSE) {
      const DWORD error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY)
        return 2;
      Sleep(backoff_ms);
      backoff_ms = next_backoff(backoff_ms);
      continue;
    }
    const HANDLE pipe = CreateFileW(pipe_name, GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      Sleep(backoff_ms);
      backoff_ms = next_backoff(backoff_ms);
      continue;
    }
    if (!daemon_sibling_from_pipe(pipe)) {
      CloseHandle(pipe);
      if (one_shot) return 6;
      Sleep(backoff_ms);
      backoff_ms = next_backoff(backoff_ms);
      continue;
    }
    backoff_ms = 100;
    for (;;) {
      std::array<std::byte, layerc::core::frame_header_bytes> header{};
      if (!read_exact(pipe, header.data(),
                      static_cast<DWORD>(header.size())))
        break;
      const auto length = layerc::core::read_frame_header(header.data());
      if (length > layerc::core::max_companion_snapshot_bytes) {
        CloseHandle(pipe);
        return 3;
      }
      std::string payload(length, '\0');
      if (!read_exact(pipe, payload.data(), length)) break;
      const auto parsed = layerc::core::parse_companion_snapshot_json(payload);
      if (std::holds_alternative<layerc::core::CompanionError>(parsed)) {
        CloseHandle(pipe);
        return 4;
      }
      if (one_shot) {
        CloseHandle(pipe);
        return 0;
      }
    }
    CloseHandle(pipe);
    // One-shot: a connection that ended without a usable frame is a failure
    // (exit 5) rather than an endless reconnect.
    if (one_shot) return 5;
  }
}

} // namespace

int WINAPI wWinMain(const HINSTANCE, const HINSTANCE, const PWSTR, const int) {
  return run_host();
}
