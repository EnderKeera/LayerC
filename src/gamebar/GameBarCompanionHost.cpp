#include "layerc/core/companion.hpp"
#include "layerc/core/frame.hpp"

#include <windows.h>
#include <winrt/Windows.ApplicationModel.AppService.h>
#include <winrt/base.h>

#include <array>
#include <cstddef>
#include <string>

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

  for (;;) {
    if (WaitNamedPipeW(pipe_name, 2'000) == FALSE) {
      const DWORD error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_BUSY) continue;
      return 2;
    }
    const HANDLE pipe = CreateFileW(pipe_name, GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) continue;
    bool received{};
    for (;;) {
      std::array<std::byte, layerc::core::frame_header_bytes> header{};
      if (!read_exact(pipe, header.data(), static_cast<DWORD>(header.size())))
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
      received = true;
      if (one_shot) {
        CloseHandle(pipe);
        return received ? 0 : 5;
      }
    }
    CloseHandle(pipe);
  }
}

} // namespace

int WINAPI wWinMain(const HINSTANCE, const HINSTANCE, const PWSTR, const int) {
  return run_host();
}
