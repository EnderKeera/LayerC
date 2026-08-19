#include "GameBarTransport.hpp"

#include "layerc/core/frame.hpp"

#include <cstring>
#include <sddl.h>
#include <appmodel.h>
#include <vector>

namespace layerc::gamebar {
namespace {

std::wstring windows_error_text(const DWORD error) {
  wchar_t *message{};
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
  std::wstring result = length != 0 && message != nullptr
                            ? std::wstring(message, length)
                            : L"Windows error " + std::to_wstring(error);
  if (message != nullptr) LocalFree(message);
  while (!result.empty() &&
         (result.back() == L'\r' || result.back() == L'\n' ||
          result.back() == L' '))
    result.pop_back();
  return result;
}

PSECURITY_DESCRIPTOR current_user_descriptor() {
  HANDLE token{};
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
    return nullptr;
  DWORD required{};
  (void)GetTokenInformation(token, TokenUser, nullptr, 0, &required);
  std::vector<std::byte> storage(required);
  PSECURITY_DESCRIPTOR descriptor{};
  if (required != 0 &&
      GetTokenInformation(token, TokenUser, storage.data(), required,
                          &required) != FALSE) {
    const auto *user = reinterpret_cast<const TOKEN_USER *>(storage.data());
    LPWSTR sid_string{};
    if (ConvertSidToStringSidW(user->User.Sid, &sid_string) != FALSE &&
        sid_string != nullptr) {
      const std::wstring sddl =
          L"D:P(A;;GA;;;" + std::wstring(sid_string) + L")(A;;GA;;;SY)";
      (void)ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr);
      LocalFree(sid_string);
    }
  }
  CloseHandle(token);
  return descriptor;
}

bool same_session(const DWORD process_id) {
  DWORD current{};
  DWORD peer{};
  return ProcessIdToSessionId(GetCurrentProcessId(), &current) != FALSE &&
         ProcessIdToSessionId(process_id, &peer) != FALSE && current == peer;
}

bool is_gamebar_package(const HANDLE process) {
  UINT32 length{};
  if (GetPackageId(process, &length, nullptr) !=
          ERROR_INSUFFICIENT_BUFFER ||
      length < sizeof(PACKAGE_ID))
    return false;
  std::vector<std::byte> storage(length);
  auto *package = reinterpret_cast<PACKAGE_ID *>(storage.data());
  if (GetPackageId(process, &length,
                  reinterpret_cast<BYTE *>(package)) != ERROR_SUCCESS ||
      package->name == nullptr)
    return false;
  return GameBarTransport::package_name ==
         std::wstring_view(package->name);
}

} // namespace

GameBarTransport::~GameBarTransport() { stop(); }

bool GameBarTransport::start() {
  if (started_) return true;
  started_ = true;
  connect_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (connect_event_ == nullptr || write_event_ == nullptr) {
    set_error(GetLastError());
    stop();
    return false;
  }
  if (!create_server()) {
    stop();
    return false;
  }
  return true;
}

void GameBarTransport::stop() noexcept {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    CancelIoEx(pipe_, nullptr);
    DisconnectNamedPipe(pipe_);
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
  if (connect_event_ != nullptr) CloseHandle(connect_event_);
  if (write_event_ != nullptr) CloseHandle(write_event_);
  connect_event_ = nullptr;
  write_event_ = nullptr;
  connect_overlapped_ = {};
  write_overlapped_ = {};
  started_ = false;
  connect_pending_ = false;
  write_pending_ = false;
  connected_ = false;
  active_frame_.clear();
  pending_frame_.clear();
}

void GameBarTransport::set_error(const DWORD error) {
  last_error_ = windows_error_text(error);
}

bool GameBarTransport::create_server() {
  if (pipe_ != INVALID_HANDLE_VALUE) return true;
  PSECURITY_DESCRIPTOR descriptor = current_user_descriptor();
  if (descriptor == nullptr) {
    last_error_ = L"Could not create the Game Bar current-user security descriptor.";
    return false;
  }
  SECURITY_ATTRIBUTES security{.nLength = sizeof(SECURITY_ATTRIBUTES),
                               .lpSecurityDescriptor = descriptor,
                               .bInheritHandle = FALSE};
  pipe_ = CreateNamedPipeW(
      pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      1, static_cast<DWORD>(max_frame_bytes + layerc::core::frame_header_bytes),
      static_cast<DWORD>(max_frame_bytes + layerc::core::frame_header_bytes),
      0, &security);
  LocalFree(descriptor);
  if (pipe_ == INVALID_HANDLE_VALUE) {
    set_error(GetLastError());
    return false;
  }
  ResetEvent(connect_event_);
  connect_overlapped_ = {};
  connect_overlapped_.hEvent = connect_event_;
  const BOOL result = ConnectNamedPipe(pipe_, &connect_overlapped_);
  if (result != FALSE) {
    complete_connection();
    return true;
  }
  const DWORD error = GetLastError();
  if (error == ERROR_IO_PENDING) {
    connect_pending_ = true;
    return true;
  }
  if (error == ERROR_PIPE_CONNECTED) {
    complete_connection();
    return true;
  }
  set_error(error);
  disconnect();
  return false;
}

bool GameBarTransport::authenticate_client() {
  ULONG process_id{};
  if (GetNamedPipeClientProcessId(pipe_, &process_id) == FALSE ||
      process_id == 0 || !same_session(process_id))
    return false;
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (process == nullptr) return false;
  const bool trusted = is_gamebar_package(process);
  CloseHandle(process);
  return trusted;
}

void GameBarTransport::complete_connection() noexcept {
  connect_pending_ = false;
  if (!authenticate_client()) {
    disconnect();
    return;
  }
  connected_ = true;
  last_error_.clear();
  (void)continue_write();
}

void GameBarTransport::disconnect() noexcept {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    CancelIoEx(pipe_, nullptr);
    DisconnectNamedPipe(pipe_);
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
  connect_pending_ = false;
  write_pending_ = false;
  connected_ = false;
  active_frame_.clear();
}

bool GameBarTransport::publish(const std::string_view snapshot_json) {
  if (snapshot_json.size() > max_frame_bytes) return false;
  pending_frame_.resize(layerc::core::frame_header_bytes + snapshot_json.size());
  layerc::core::append_frame_header(
      pending_frame_.data(), static_cast<std::uint32_t>(snapshot_json.size()));
  std::memcpy(pending_frame_.data() + layerc::core::frame_header_bytes,
              snapshot_json.data(), snapshot_json.size());
  if (!connected_) return true;
  return continue_write();
}

bool GameBarTransport::continue_write() {
  if (!connected_ || write_pending_) return true;
  if (active_frame_.empty()) {
    if (pending_frame_.empty()) return true;
    active_frame_ = std::move(pending_frame_);
    pending_frame_.clear();
  }
  ResetEvent(write_event_);
  write_overlapped_ = {};
  write_overlapped_.hEvent = write_event_;
  DWORD written{};
  const BOOL result = WriteFile(
      pipe_, active_frame_.data(), static_cast<DWORD>(active_frame_.size()),
      &written, &write_overlapped_);
  if (result != FALSE) {
    if (written != active_frame_.size()) {
      disconnect();
      return false;
    }
    active_frame_.clear();
    return continue_write();
  }
  const DWORD error = GetLastError();
  if (error == ERROR_IO_PENDING) {
    write_pending_ = true;
    return true;
  }
  set_error(error);
  disconnect();
  return false;
}

void GameBarTransport::poll() noexcept {
  if (!started_) return;
  if (pipe_ == INVALID_HANDLE_VALUE) {
    (void)create_server();
    return;
  }
  if (connect_pending_ &&
      WaitForSingleObject(connect_event_, 0) == WAIT_OBJECT_0) {
    DWORD transferred{};
    if (GetOverlappedResult(pipe_, &connect_overlapped_, &transferred,
                            FALSE))
      complete_connection();
    else {
      set_error(GetLastError());
      disconnect();
    }
  }
  if (write_pending_ &&
      WaitForSingleObject(write_event_, 0) == WAIT_OBJECT_0) {
    DWORD written{};
    if (GetOverlappedResult(pipe_, &write_overlapped_, &written, FALSE) &&
        written == active_frame_.size()) {
      write_pending_ = false;
      active_frame_.clear();
      (void)continue_write();
    } else {
      set_error(GetLastError());
      disconnect();
    }
  }
  if (connected_) {
    DWORD available{};
    if (PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr) == FALSE)
      disconnect();
  } else if (pipe_ == INVALID_HANDLE_VALUE) {
    (void)create_server();
  }
}

HANDLE GameBarTransport::wait_handle() const noexcept {
  if (connect_pending_) return connect_event_;
  if (write_pending_) return write_event_;
  return nullptr;
}

std::size_t GameBarTransport::resource_handle_count() const noexcept {
  std::size_t count = 0;
  if (pipe_ != INVALID_HANDLE_VALUE) ++count;
  if (connect_event_ != nullptr) ++count;
  if (write_event_ != nullptr) ++count;
  return count;
}

} // namespace layerc::gamebar
