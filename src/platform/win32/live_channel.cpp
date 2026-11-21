#include "layerc/platform/live_channel.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/win32_error.hpp"
#include "layerc/platform/win32_handle.hpp"
#include "layerc/platform/window_query.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <vector>

#include <sddl.h>

namespace layerc::platform {
namespace {

HANDLE make_event() {
  return CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

// The peer is the opposite pipe end: LayerD's server for a client channel and
// LayerE's client for a server channel.
[[nodiscard]] bool peer_is_server(
    const NamedPipeEditorDaemonChannel::Role role) noexcept {
  return role == NamedPipeEditorDaemonChannel::Role::client;
}

} // namespace

bool is_sibling_editor_process(const DWORD process_id) {
  return process_id != 0 &&
         std::ranges::any_of(layerc_editor_image_names, [&](const wchar_t *image) {
           return is_sibling_process_image(process_id, image);
         });
}

bool is_layerc_editor_process(const DWORD process_id,
                              const DWORD authenticated_editor_id) {
  if (process_id == 0) return false;
  if (authenticated_editor_id != 0 && process_id == authenticated_editor_id)
    return true;
  return is_sibling_editor_process(process_id);
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
  close_handles();
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
  if (!authenticate_peer()) {
    close_handles();
    return false;
  }
  complete_connection();
  return true;
}

bool NamedPipeEditorDaemonChannel::authenticate_peer() {
  const auto verified = verify_peer();
  if (const auto *peer_pid = std::get_if<DWORD>(&verified)) {
    peer_process_id_ = *peer_pid;
    return true;
  }
  const bool server = peer_is_server(role_);
  const std::wstring product = server ? L"LayerD" : L"LayerE";
  const std::wstring peer = server ? L"server" : L"client";
  switch (std::get<PeerFault>(verified)) {
  case PeerFault::identity:
    last_error_ = L"Could not identify the " + product + L" live-channel " +
                  peer + L".";
    break;
  case PeerFault::unexpected:
    last_error_ = L"Rejected an unexpected live-channel " + peer + L" process.";
    break;
  case PeerFault::session:
    last_error_ =
        L"Rejected a live-channel " + peer + L" from another logon session.";
    break;
  case PeerFault::authenticate:
    last_error_ = L"Could not authenticate the " + product +
                  L" live-channel " + peer + L".";
    break;
  case PeerFault::untrusted:
    last_error_ = L"Rejected an untrusted " + product + L" live-channel " +
                  peer + L".";
    break;
  }
  return false;
}

std::variant<DWORD, NamedPipeEditorDaemonChannel::PeerFault>
NamedPipeEditorDaemonChannel::verify_peer() const {
  const bool server = peer_is_server(role_);
  ULONG peer_pid{};
  const BOOL identified = server
      ? GetNamedPipeServerProcessId(pipe_, &peer_pid)
      : GetNamedPipeClientProcessId(pipe_, &peer_pid);
  if (identified == FALSE || peer_pid == 0)
    return PeerFault::identity;
  if (expected_peer_process_id_ != 0 && peer_pid != expected_peer_process_id_)
    return PeerFault::unexpected;
  if (!same_session_as_current_process(peer_pid))
    return PeerFault::session;
  const OwnedHandle peer_process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, peer_pid));
  if (!peer_process.valid())
    return PeerFault::authenticate;
  const bool trusted_user = same_user_as_current_process(peer_process.get());
  const bool trusted_image =
      expected_peer_process_id_ != 0 ||
      (server ? is_sibling_process_image(peer_pid, L"layerd.exe")
              : is_sibling_editor_process(peer_pid));
  if (!trusted_user || !trusted_image)
    return PeerFault::untrusted;
  return DWORD{peer_pid};
}

void NamedPipeEditorDaemonChannel::complete_connection() {
  if (role_ == Role::server && !authenticate_peer()) {
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
  decoder_.clear();
  pending_messages_.clear();
  // This object lives for LayerD's entire lifetime. clear() would keep the
  // largest profile frame and deque storage committed after LayerE closes.
  std::vector<std::byte>().swap(active_write_);
  std::deque<std::vector<std::byte>>().swap(write_queue_);
  if (was_connected) connection_changed_ = true;
  if (error != ERROR_SUCCESS && error != ERROR_BROKEN_PIPE &&
      error != ERROR_PIPE_NOT_CONNECTED && error != ERROR_OPERATION_ABORTED)
    set_error(error);
}

void NamedPipeEditorDaemonChannel::close_handles() noexcept {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    // Cancellation is only a request (including ERROR_NOT_FOUND when an I/O
    // just completed). Keep the pipe, events, OVERLAPPEDs, and both buffers
    // alive until every submitted operation has reached a terminal result.
    (void)CancelIoEx(pipe_, nullptr);
    const auto drain = [&](OVERLAPPED &overlapped, bool &pending) {
      if (!pending) return;
      DWORD transferred{};
      (void)GetOverlappedResult(pipe_, &overlapped, &transferred, TRUE);
      pending = false;
    };
    drain(connect_overlapped_, connect_pending_);
    drain(read_overlapped_, read_pending_);
    drain(write_overlapped_, write_pending_);
    if (role_ == Role::server) (void)DisconnectNamedPipe(pipe_);
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
  for (std::size_t writes = 0; writes < max_messages_per_poll; ++writes) {
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
      continue;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {
      write_pending_ = true;
      return;
    }
    disconnect(error);
    return;
  }
}

void NamedPipeEditorDaemonChannel::drain_messages(
    std::vector<layerc::core::LiveMessageResult> &messages) {
  while (messages.size() < max_messages_per_poll && !pending_messages_.empty()) {
    messages.push_back(std::move(pending_messages_.front()));
    pending_messages_.pop_front();
  }
}

void NamedPipeEditorDaemonChannel::queue_decoded(
    std::vector<layerc::core::LiveMessageResult> decoded,
    std::vector<layerc::core::LiveMessageResult> &messages) {
  for (auto &message : decoded) pending_messages_.push_back(std::move(message));
  drain_messages(messages);
}

void NamedPipeEditorDaemonChannel::start_read(
    std::vector<layerc::core::LiveMessageResult> &messages) {
  for (std::size_t reads = 0; reads < max_messages_per_poll; ++reads) {
    if (!connected_ || read_pending_ || pipe_ == INVALID_HANDLE_VALUE ||
        messages.size() >= max_messages_per_poll || !pending_messages_.empty())
      return;
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
      queue_decoded(decoder_.push(
          std::span<const std::byte>(read_buffer_.data(), read)), messages);
      continue;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {
      read_pending_ = true;
      return;
    }
    disconnect(error);
    return;
  }
}

bool NamedPipeEditorDaemonChannel::service_connect() {
  if (!connect_pending_ ||
      WaitForSingleObject(connect_event_, 0) != WAIT_OBJECT_0)
    return true;
  DWORD transferred{};
  if (GetOverlappedResult(pipe_, &connect_overlapped_, &transferred, FALSE))
    complete_connection();
  else
    disconnect(GetLastError());
  return connected_;
}

bool NamedPipeEditorDaemonChannel::service_write() {
  if (write_pending_ && WaitForSingleObject(write_event_, 0) == WAIT_OBJECT_0) {
    DWORD written{};
    if (!GetOverlappedResult(pipe_, &write_overlapped_, &written, FALSE) ||
        written != active_write_.size()) {
      disconnect(GetLastError());
      return false;
    }
    write_pending_ = false;
    active_write_.clear();
  }
  continue_write();
  return connected_;
}

bool NamedPipeEditorDaemonChannel::service_read(
    std::vector<layerc::core::LiveMessageResult> &messages) {
  if (read_pending_ && WaitForSingleObject(read_event_, 0) == WAIT_OBJECT_0) {
    DWORD read{};
    if (!GetOverlappedResult(pipe_, &read_overlapped_, &read, FALSE) ||
        read == 0) {
      disconnect(GetLastError());
      return false;
    }
    read_pending_ = false;
    queue_decoded(decoder_.push(
        std::span<const std::byte>(read_buffer_.data(), read)), messages);
  }
  start_read(messages);
  return connected_;
}

std::vector<layerc::core::LiveMessageResult>
NamedPipeEditorDaemonChannel::poll() {
  std::vector<layerc::core::LiveMessageResult> messages;
  drain_messages(messages);
  const auto relisten_server = [&] {
    if (role_ == Role::server && pipe_ == INVALID_HANDLE_VALUE)
      (void)create_server_pipe();
  };
  relisten_server();
  if (role_ == Role::client && pipe_ == INVALID_HANDLE_VALUE)
    (void)connect_client();

  (void)service_connect();
  if (!connected_) {
    // Re-arm once after a rejected/closed peer in this poll. This is outside
    // create_server_pipe's call stack, so a spoofing client cannot force
    // recursive reconnects, while a legitimate editor avoids timer jitter.
    relisten_server();
    if (!connected_) return messages;
  }

  if (!service_write() || !service_read(messages)) relisten_server();
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
  // A poll capped at max_messages_per_poll (or a synchronous reconnect) can
  // leave read or write work with no overlapped I/O in flight. Signal the idle
  // manual-reset event so the owner's wait returns for the next poll instead
  // of stalling; issuing the next overlapped operation resets it.
  const bool read_deferred = connected_ && !read_pending_;
  const bool write_deferred =
      connected_ && !write_pending_ &&
      (!active_write_.empty() || !write_queue_.empty());
  if (read_deferred) SetEvent(read_event_);
  if (write_deferred) SetEvent(write_event_);
  append(connect_pending_, connect_event_);
  append(read_pending_ || read_deferred, read_event_);
  append(write_pending_ || write_deferred, write_event_);
  return count;
}

} // namespace layerc::platform
