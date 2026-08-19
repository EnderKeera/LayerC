#include "layerc/core/companion.hpp"
#include "layerc/core/frame.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr wchar_t pipe_name[] = L"\\\\.\\pipe\\LayerC.GameBar.v1";

bool write_all(const HANDLE pipe, const void *source, const DWORD bytes) {
  const auto *cursor = static_cast<const std::byte *>(source);
  DWORD remaining = bytes;
  while (remaining != 0) {
    DWORD written{};
    if (WriteFile(pipe, cursor, remaining, &written, nullptr) == FALSE ||
        written == 0)
      return false;
    cursor += written;
    remaining -= written;
  }
  return true;
}

} // namespace

int main(const int argc, char **argv) {
  if (argc != 2) return 2;
  SetEnvironmentVariableW(L"LAYERC_GAMEBAR_ONESHOT", L"1");
  const HANDLE server = CreateNamedPipeW(
      pipe_name, PIPE_ACCESS_OUTBOUND,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
      static_cast<DWORD>(layerc::core::max_companion_snapshot_bytes),
      static_cast<DWORD>(layerc::core::max_companion_snapshot_bytes), 0,
      nullptr);
  if (server == INVALID_HANDLE_VALUE) return 3;

  const auto executable = std::filesystem::path(argv[1]);
  std::wstring command = L"\"" + executable.wstring() + L"\"";
  std::vector<wchar_t> command_line(command.begin(), command.end());
  command_line.push_back(L'\0');
  STARTUPINFOW startup{.cb = sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process) == FALSE) {
    CloseHandle(server);
    return 4;
  }
  CloseHandle(process.hThread);

  const BOOL connected = ConnectNamedPipe(server, nullptr);
  if (connected == FALSE && GetLastError() != ERROR_PIPE_CONNECTED) {
    TerminateProcess(process.hProcess, 5);
    CloseHandle(process.hProcess);
    CloseHandle(server);
    return 5;
  }

  const std::string payload =
      R"({"profileId":"p","sessionKey":"s","timestampMs":1,"panels":[]})";
  std::array<std::byte, layerc::core::frame_header_bytes> header{};
  layerc::core::append_frame_header(header.data(),
                                    static_cast<std::uint32_t>(payload.size()));
  const bool sent =
      write_all(server, header.data(), static_cast<DWORD>(header.size())) &&
      write_all(server, payload.data(), static_cast<DWORD>(payload.size()));
  FlushFileBuffers(server);
  DisconnectNamedPipe(server);
  CloseHandle(server);

  const DWORD wait = WaitForSingleObject(process.hProcess, 5'000);
  DWORD exit_code{1};
  if (wait == WAIT_OBJECT_0)
    GetExitCodeProcess(process.hProcess, &exit_code);
  else
    TerminateProcess(process.hProcess, 6);
  CloseHandle(process.hProcess);
  return sent && wait == WAIT_OBJECT_0 && exit_code == 0 ? 0 : 7;
}
