#include "layerc/platform/live_channel.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/windows.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <span>
#include <vector>

#include <sddl.h>

namespace layerc::platform {
namespace {

HANDLE make_event() {
  return CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

bool same_user_as_current_process(const HANDLE process) {
  HANDLE current_token{};
  HANDLE server_token{};
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &current_token) ==
          FALSE ||
      OpenProcessToken(process, TOKEN_QUERY, &server_token) == FALSE) {
    if (current_token != nullptr) CloseHandle(current_token);
    if (server_token != nullptr) CloseHandle(server_token);
    return false;
  }

  const auto token_user = [](const HANDLE token,
                             std::vector<std::byte> &storage) -> PSID {
    DWORD required{};
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
      return nullptr;
    storage.resize(required);
    if (GetTokenInformation(token, TokenUser, storage.data(), required,
                            &required) == FALSE)
      return nullptr;
    return reinterpret_cast<TOKEN_USER *>(storage.data())->User.Sid;
  };

  std::vector<std::byte> current_storage;
  std::vector<std::byte> server_storage;
  const PSID current_sid = token_user(current_token, current_storage);
  const PSID server_sid = token_user(server_token, server_storage);
  const bool matches = current_sid != nullptr && server_sid != nullptr &&
                       EqualSid(current_sid, server_sid) != FALSE;
  CloseHandle(server_token);
  CloseHandle(current_token);
  return matches;
}

bool is_sibling_image(const HANDLE process, const wchar_t *const filename) {
  std::array<wchar_t, 32'768> editor_path{};
  const DWORD editor_length = GetModuleFileNameW(
      nullptr, editor_path.data(), static_cast<DWORD>(editor_path.size()));
  if (editor_length == 0 || editor_length >= editor_path.size()) return false;

  std::array<wchar_t, 32'768> server_path{};
  DWORD server_length = static_cast<DWORD>(server_path.size());
  if (QueryFullProcessImageNameW(process, 0, server_path.data(),
                                 &server_length) == FALSE ||
      server_length == 0)
    return false;

  const auto expected =
      std::filesystem::path(
          std::wstring_view(editor_path.data(), editor_length))
          .parent_path() /
      filename;
  const auto actual = std::filesystem::path(
      std::wstring_view(server_path.data(), server_length));
  std::error_code error;
  const bool equivalent = std::filesystem::equivalent(expected, actual, error);
  return !error && equivalent;
}

} // namespace

bool is_sibling_process_image(const DWORD process_id,
                              const wchar_t *filename) {
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (process == nullptr) return false;
  const bool sibling = is_sibling_image(process, filename);
  CloseHandle(process);
  return sibling;
}

NamedPipeEditorDaemonChannel::NamedPipeEditorDaemonChannel(
    const Role role, std::wstring pipe_name,
    const DWORD expected_peer_process_id)
    : role_(role), pipe_name_(std::move(pipe_name)),
      expected_peer_process_id_(expected_peer_process_id),
      connect_event_(make_event()), read_event_(make_event()),
      write_event_(make_event()) {
  connect_overlapped_.hEvent = connect_event_;
  read_overlapped_.hEvent = read_event_;
  write_overlapped_.hEvent = write_event_;
  if (role_ == Role::server) (void)create_server_pipe();
}

NamedPipeEditorDaemonChannel::~NamedPipeEditorDaemonChannel() {
  close_handles();
  for (auto *event : {connect_event_, read_event_, write_event_}) {
    if (event != nullptr) CloseHandle(event);
  }
}

void NamedPipeEditorDaemonChannel::set_error(const DWORD error) {
  last_error_ = windows_error_text(error);
}

bool NamedPipeEditorDaemonChannel::create_server_pipe() {
  if (pipe_ != INVALID_HANDLE_VALUE) return true;
  PSECURITY_DESCRIPTOR descriptor{};
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;GA;;;OW)(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor,
          nullptr) == FALSE) {
    set_error(GetLastError());
    return false;
  }
  SECURITY_ATTRIBUTES security{.nLength = sizeof(SECURITY_ATTRIBUTES),
                               .lpSecurityDescriptor = descriptor,
                               .bInheritHandle = FALSE};
  pipe_ = CreateNamedPipeW(
      pipe_name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
      static_cast<DWORD>(io_buffer_size), static_cast<DWORD>(io_buffer_size),
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
  CloseHandle(pipe_);
  pipe_ = INVALID_HANDLE_VALUE;
  return false;
}

bool NamedPipeEditorDaemonChannel::connect_client() {
  if (pipe_ != INVALID_HANDLE_VALUE) return connected_;
  const ULONGLONG now = GetTickCount64();
  if (now < next_client_attempt_) return false;
  next_client_attempt_ = now + 250;
  pipe_ = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                      nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  if (pipe_ == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY)
      set_error(error);
    return false;
  }
  if (!authenticate_server()) {
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
    return false;
  }
  complete_connection();
  return true;
}

bool NamedPipeEditorDaemonChannel::authenticate_server() {
  ULONG server_process_id{};
  if (GetNamedPipeServerProcessId(pipe_, &server_process_id) == FALSE ||
      server_process_id == 0) {
    last_error_ = L"Could not identify the LayerD live-channel server.";
    return false;
  }
  if (expected_peer_process_id_ != 0 &&
      server_process_id != expected_peer_process_id_) {
    last_error_ = L"Rejected an unexpected live-channel server process.";
    return false;
  }
  return verify_peer_identity(
      server_process_id, L"layerd.exe",
      L"Rejected a live-channel server from another logon session.",
      L"Could not authenticate the LayerD live-channel server.",
      L"Rejected an untrusted LayerD live-channel server.");
}

bool NamedPipeEditorDaemonChannel::authenticate_client() {
  ULONG client_process_id{};
  if (GetNamedPipeClientProcessId(pipe_, &client_process_id) == FALSE ||
      client_process_id == 0) {
    last_error_ = L"Could not identify the LayerE live-channel client.";
    return false;
  }
  if (expected_peer_process_id_ != 0 &&
      client_process_id != expected_peer_process_id_) {
    last_error_ = L"Rejected an unexpected live-channel client process.";
    return false;
  }
  return verify_peer_identity(
      client_process_id, L"layere.exe",
      L"Rejected a live-channel client from another logon session.",
      L"Could not authenticate the LayerE live-channel client.",
      L"Rejected an untrusted LayerE live-channel client.");
}

bool NamedPipeEditorDaemonChannel::verify_peer_identity(
    const DWORD peer_pid, const wchar_t *sibling_image,
    const std::wstring_view session_error,
    const std::wstring_view authenticate_error,
    const std::wstring_view untrusted_error) {
  DWORD current_session{};
  DWORD peer_session{};
  if (ProcessIdToSessionId(GetCurrentProcessId(), &current_session) == FALSE ||
      ProcessIdToSessionId(peer_pid, &peer_session) == FALSE ||
      current_session != peer_session) {
    last_error_ = std::wstring(session_error);
    return false;
  }
  const HANDLE peer_process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, peer_pid);
  if (peer_process == nullptr) {
    last_error_ = std::wstring(authenticate_error);
    return false;
  }
  const bool trusted_user = same_user_as_current_process(peer_process);
  const bool trusted_image = expected_peer_process_id_ != 0 ||
                             is_sibling_image(peer_process, sibling_image);
  CloseHandle(peer_process);
  if (!trusted_user || !trusted_image) {
    last_error_ = std::wstring(untrusted_error);
    return false;
  }
  peer_process_id_ = peer_pid;
  return true;
}

void NamedPipeEditorDaemonChannel::complete_connection() {
  if (role_ == Role::server && !authenticate_client()) {
    disconnect(ERROR_SUCCESS);
    return;
  }
  connect_pending_ = false;
  connected_ = true;
  connection_changed_ = true;
  last_error_.clear();
  decoder_.clear();
}

void NamedPipeEditorDaemonChannel::disconnect(const DWORD error) {
  close_handles();
  const bool was_connected = connected_;
  connected_ = false;
  peer_process_id_ = 0;
  connect_pending_ = false;
  read_pending_ = false;
  write_pending_ = false;
  decoder_.clear();
  // This object lives for LayerD's entire lifetime. clear() would keep the
  // largest profile frame and deque storage committed after LayerE closes.
  std::vector<std::byte>().swap(active_write_);
  std::deque<std::vector<std::byte>>().swap(write_queue_);
  if (was_connected) connection_changed_ = true;
  if (error != ERROR_SUCCESS && error != ERROR_BROKEN_PIPE &&
      error != ERROR_PIPE_NOT_CONNECTED && error != ERROR_OPERATION_ABORTED)
    set_error(error);
}

void NamedPipeEditorDaemonChannel::close_handles() {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    CancelIoEx(pipe_, nullptr);
    if (role_ == Role::server) DisconnectNamedPipe(pipe_);
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
}

bool NamedPipeEditorDaemonChannel::send(
    const layerc::core::LiveMessage &message) {
  auto encoded = layerc::core::encode_live_frame(message);
  const auto *frame = std::get_if<std::vector<std::byte>>(&encoded);
  if (frame == nullptr) {
    last_error_ = layerc::platform::utf8_to_wide(
        std::get<layerc::core::LiveProtocolError>(encoded).message);
    return false;
  }
  std::size_t queued = active_write_.size();
  for (const auto &pending : write_queue_) queued += pending.size();
  if (queued + frame->size() > layerc::core::max_live_message_bytes * 4U) {
    last_error_ = L"Live channel write queue is full.";
    return false;
  }
  write_queue_.push_back(std::move(*frame));
  if (connected_) continue_write();
  return true;
}

void NamedPipeEditorDaemonChannel::continue_write() {
  if (!connected_ || write_pending_ || pipe_ == INVALID_HANDLE_VALUE) return;
  if (active_write_.empty()) {
    if (write_queue_.empty()) return;
    active_write_ = std::move(write_queue_.front());
    write_queue_.pop_front();
  }
  ResetEvent(write_event_);
  write_overlapped_ = {};
  write_overlapped_.hEvent = write_event_;
  DWORD written{};
  const BOOL result = WriteFile(pipe_, active_write_.data(),
                                static_cast<DWORD>(active_write_.size()),
                                &written, &write_overlapped_);
  if (result != FALSE) {
    if (written != active_write_.size()) {
      disconnect(ERROR_WRITE_FAULT);
      return;
    }
    active_write_.clear();
    continue_write();
    return;
  }
  const DWORD error = GetLastError();
  if (error == ERROR_IO_PENDING) {
    write_pending_ = true;
    return;
  }
  disconnect(error);
}

void NamedPipeEditorDaemonChannel::start_read(
    std::vector<layerc::core::LiveMessageResult> &messages) {
  if (!connected_ || read_pending_ || pipe_ == INVALID_HANDLE_VALUE) return;
  ResetEvent(read_event_);
  read_overlapped_ = {};
  read_overlapped_.hEvent = read_event_;
  DWORD read{};
  const BOOL result = ReadFile(pipe_, read_buffer_.data(),
                               static_cast<DWORD>(read_buffer_.size()), &read,
                               &read_overlapped_);
  if (result != FALSE) {
    if (read == 0) {
      disconnect(ERROR_BROKEN_PIPE);
      return;
    }
    auto decoded = decoder_.push(
        std::span<const std::byte>(read_buffer_.data(), read));
    messages.insert(messages.end(),
                    std::make_move_iterator(decoded.begin()),
                    std::make_move_iterator(decoded.end()));
    start_read(messages);
    return;
  }
  const DWORD error = GetLastError();
  if (error == ERROR_IO_PENDING) {
    read_pending_ = true;
    return;
  }
  disconnect(error);
}

std::vector<layerc::core::LiveMessageResult>
NamedPipeEditorDaemonChannel::poll() {
  std::vector<layerc::core::LiveMessageResult> messages;
  const auto relisten_server = [&] {
    if (role_ == Role::server && pipe_ == INVALID_HANDLE_VALUE)
      (void)create_server_pipe();
  };
  if (role_ == Role::server && pipe_ == INVALID_HANDLE_VALUE)
    (void)create_server_pipe();
  if (role_ == Role::client && pipe_ == INVALID_HANDLE_VALUE)
    (void)connect_client();

  if (connect_pending_ && WaitForSingleObject(connect_event_, 0) == WAIT_OBJECT_0) {
    DWORD transferred{};
    if (GetOverlappedResult(pipe_, &connect_overlapped_, &transferred, FALSE))
      complete_connection();
    else
      disconnect(GetLastError());
  }
  if (!connected_) {
    // Re-arm once after a rejected/closed peer in this poll. This is outside
    // create_server_pipe's call stack, so a spoofing client cannot force
    // recursive reconnects, while a legitimate editor avoids timer jitter.
    relisten_server();
    if (!connected_) return messages;
  }

  if (write_pending_ && WaitForSingleObject(write_event_, 0) == WAIT_OBJECT_0) {
    DWORD written{};
    if (!GetOverlappedResult(pipe_, &write_overlapped_, &written, FALSE) ||
        written != active_write_.size()) {
      disconnect(GetLastError());
      relisten_server();
      return messages;
    }
    write_pending_ = false;
    active_write_.clear();
  }
  continue_write();
  if (!connected_) {
    relisten_server();
    return messages;
  }

  if (read_pending_ && WaitForSingleObject(read_event_, 0) == WAIT_OBJECT_0) {
    DWORD read{};
    if (!GetOverlappedResult(pipe_, &read_overlapped_, &read, FALSE) || read == 0) {
      disconnect(GetLastError());
      relisten_server();
      return messages;
    }
    read_pending_ = false;
    auto decoded = decoder_.push(
        std::span<const std::byte>(read_buffer_.data(), read));
    messages.insert(messages.end(),
                    std::make_move_iterator(decoded.begin()),
                    std::make_move_iterator(decoded.end()));
  }
  start_read(messages);
  if (!connected_) relisten_server();
  return messages;
}

bool NamedPipeEditorDaemonChannel::take_connection_changed() noexcept {
  const bool changed = connection_changed_;
  connection_changed_ = false;
  return changed;
}

void NamedPipeEditorDaemonChannel::disconnect_peer() noexcept {
  disconnect(ERROR_REVISION_MISMATCH);
}

std::size_t NamedPipeEditorDaemonChannel::wait_handles(
    const std::span<HANDLE> output) const noexcept {
  std::size_t count{};
  const auto append = [&](const bool pending, const HANDLE event) {
    if (!pending || event == nullptr || count >= output.size()) return;
    output[count++] = event;
  };
  append(connect_pending_, connect_event_);
  append(read_pending_, read_event_);
  append(write_pending_, write_event_);
  return count;
}

} // namespace layerc::platform
