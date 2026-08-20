// LayerC Game Bar host tests: exercise the host executable against a fake
// daemon-side server for the happy path and for every error exit, including
// the host's mutual-authentication requirement (layer21-F4/F6).
//
// Roles: the binary is the CTest driver (argv[1] = host exe path), a copied
// "layerd.exe" server next to the host (sibling - auth succeeds), or the
// original test exe acting as a foreign server (auth rejected).
#include "layerc/core/companion.hpp"
#include "layerc/core/frame.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <string>
#include <string_view>
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

std::wstring widen(const std::string_view text) {
  return std::wstring(text.begin(), text.end());
}

bool run_server_process(const std::filesystem::path &server_exe,
                        const char *mode,
                        const std::filesystem::path &host_exe,
                        DWORD &exit_code) {
  std::wstring command = L"\"" + server_exe.wstring() + L"\" " +
                         widen(mode) + L" \"" + host_exe.wstring() + L"\"";
  std::vector<wchar_t> command_line(command.begin(), command.end());
  command_line.push_back(L'\0');
  STARTUPINFOW startup{.cb = sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (CreateProcessW(server_exe.c_str(), command_line.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process) == FALSE)
    return false;
  CloseHandle(process.hThread);
  const DWORD wait = WaitForSingleObject(process.hProcess, 20'000);
  exit_code = 1;
  if (wait == WAIT_OBJECT_0)
    GetExitCodeProcess(process.hProcess, &exit_code);
  else
    TerminateProcess(process.hProcess, 2);
  CloseHandle(process.hProcess);
  return wait == WAIT_OBJECT_0;
}

// Server role: create the well-known pipe, launch the host (one-shot), serve
// one scenario, and pass only when the host exits with the expected code.
int run_server(const std::string_view mode, const std::wstring &host_exe_path) {
  SetEnvironmentVariableW(L"LAYERC_GAMEBAR_ONESHOT", L"1");
  HANDLE server = CreateNamedPipeW(
      pipe_name, PIPE_ACCESS_OUTBOUND,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
      static_cast<DWORD>(layerc::core::max_companion_snapshot_bytes),
      static_cast<DWORD>(layerc::core::max_companion_snapshot_bytes), 0,
      nullptr);
  if (server == INVALID_HANDLE_VALUE) return 3;

  std::vector<wchar_t> command_line(host_exe_path.begin(),
                                    host_exe_path.end());
  command_line.push_back(L'\0');
  STARTUPINFOW startup{.cb = sizeof(startup)};
  PROCESS_INFORMATION host{};
  if (CreateProcessW(host_exe_path.c_str(), command_line.data(), nullptr,
                      nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                      &startup, &host) == FALSE) {
    CloseHandle(server);
    return 4;
  }
  CloseHandle(host.hThread);
  const BOOL connected = ConnectNamedPipe(server, nullptr);
  if (connected == FALSE && GetLastError() != ERROR_PIPE_CONNECTED) {
    TerminateProcess(host.hProcess, 5);
    CloseHandle(host.hProcess);
    CloseHandle(server);
    return 5;
  }

  const auto send_frame = [&](const std::string_view payload) {
    std::array<std::byte, layerc::core::frame_header_bytes> header{};
    layerc::core::append_frame_header(
        header.data(), static_cast<std::uint32_t>(payload.size()));
    return write_all(server, header.data(),
                     static_cast<DWORD>(header.size())) &&
           write_all(server, payload.data(),
                     static_cast<DWORD>(payload.size()));
  };

  if (mode == "serve-happy" || mode == "serve-malformed") {
    const std::string payload =
        mode == "serve-happy"
            ? R"({"profileId":"p","sessionKey":"s","timestampMs":1,"panels":[]})"
            : "not-json";
    (void)send_frame(payload);
  } else if (mode == "serve-oversize") {
    std::array<std::byte, layerc::core::frame_header_bytes> header{};
    layerc::core::append_frame_header(
        header.data(),
        static_cast<std::uint32_t>(
            layerc::core::max_companion_snapshot_bytes + 1U));
    (void)write_all(server, header.data(),
                    static_cast<DWORD>(header.size()));
  } else if (mode == "serve-empty") {
    // Let the host finish connecting + authenticating (sibling layerd check)
    // before the connection ends, so it observes a connection with no frame
    // rather than an untrusted/vanished peer.
    Sleep(250);
  }
  FlushFileBuffers(server);
  DisconnectNamedPipe(server);
  CloseHandle(server);

  const DWORD wait = WaitForSingleObject(host.hProcess, 5'000);
  DWORD exit_code{1};
  bool exited = wait == WAIT_OBJECT_0;
  if (exited)
    GetExitCodeProcess(host.hProcess, &exit_code);
  else
    TerminateProcess(host.hProcess, 6);
  CloseHandle(host.hProcess);

  // Expected host exit per scenario; every scenario must verify it.
  const DWORD expected =
      mode == "serve-happy"    ? 0
      : mode == "serve-oversize"  ? 3
      : mode == "serve-malformed" ? 4
      : mode == "serve-empty"     ? 5
      : mode == "serve-reject"    ? 6
                                  : 0;
  if (!exited)
    std::cout << mode << ": host did not exit" << std::endl;
  else if (exit_code != expected)
    std::cout << mode << ": unexpected host exit " << exit_code
              << " (expected " << expected << ")" << std::endl;
  return exited && exit_code == expected ? 0 : 1;
}

// Driver: stage a sibling layerd.exe next to a host copy and run each
// scenario against a fresh pipe.
int run_driver(const std::filesystem::path &host_exe) {
  const auto root = std::filesystem::temp_directory_path() /
                    (L"LayerC-gamebar-host-" +
                     std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64()));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  if (error) return 2;

  std::array<wchar_t, 32'768> own_executable{};
  const DWORD own_length = GetModuleFileNameW(
      nullptr, own_executable.data(),
      static_cast<DWORD>(own_executable.size()));
  if (own_length == 0 || own_length >= own_executable.size())
    return 2;

  const auto host_copy = root / L"layerc-gamebar.exe";
  const auto daemon_copy = root / L"layerd.exe";
  const bool staged =
      CopyFileW(host_exe.c_str(), host_copy.c_str(), FALSE) != FALSE &&
      CopyFileW(own_executable.data(), daemon_copy.c_str(), FALSE) != FALSE;
  if (!staged) return 2;

  int pass_count = 0;
  const int total = 5;
  const auto scenario = [&](const char *mode, const std::filesystem::path &server,
                            const std::filesystem::path &host) {
    DWORD code = 1;
    const bool ok = run_server_process(server, mode, host, code) && code == 0;
    if (ok) ++pass_count;
    if (ok)
      std::cout << mode << ": OK" << std::endl;
    else
      std::cout << mode << ": FAIL (server exit " << code << ")" << std::endl;
  };
  // Sibling server (layerd.exe copy next to the host): auth must succeed.
  scenario("serve-happy", daemon_copy, host_copy);
  scenario("serve-oversize", daemon_copy, host_copy);
  scenario("serve-malformed", daemon_copy, host_copy);
  scenario("serve-empty", daemon_copy, host_copy);
  // Foreign server (the original test exe, not a sibling): auth must reject.
  const auto foreign = std::filesystem::path(own_executable.data());
  scenario("serve-reject", foreign, host_copy);

  std::filesystem::remove_all(root, error);
  return pass_count == total ? 0 : 1;
}

} // namespace

int main(const int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]).substr(0, 5) == "serve")
    return run_server(argv[1], std::wstring(argv[2], argv[2] + std::strlen(argv[2])));
  if (argc == 2)
    return run_driver(std::filesystem::path(argv[1]));
  return 2;
}
