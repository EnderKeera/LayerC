#pragma once

#include "layerc/core/live_edit.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

#include <windows.h>

namespace layerc::platform {

inline constexpr wchar_t live_edit_pipe_name[] =
    L"\\\\.\\pipe\\LayerC.LayerD.LiveEdit.v3";

// True for the authenticated live-edit peer or the sibling LayerC editor image.
// The product image name stays at the authentication / launch boundary.
[[nodiscard]] bool is_layerc_editor_process(DWORD process_id,
                                            DWORD authenticated_editor_id);

class NamedPipeEditorDaemonChannel final {
public:
  enum class Role { server, client };

  explicit NamedPipeEditorDaemonChannel(
      Role role, std::wstring pipe_name = live_edit_pipe_name,
      DWORD expected_peer_process_id = 0);
  ~NamedPipeEditorDaemonChannel();
  NamedPipeEditorDaemonChannel(const NamedPipeEditorDaemonChannel &) = delete;
  NamedPipeEditorDaemonChannel &
  operator=(const NamedPipeEditorDaemonChannel &) = delete;

  [[nodiscard]] bool connected() const noexcept { return connected_; }
  [[nodiscard]] bool send(const layerc::core::LiveMessage &message);
  [[nodiscard]] std::vector<layerc::core::LiveMessageResult> poll();
  [[nodiscard]] bool take_connection_changed() noexcept;
  void disconnect_peer() noexcept;
  [[nodiscard]] std::size_t
  wait_handles(std::span<HANDLE> output) const noexcept;
  [[nodiscard]] DWORD peer_process_id() const noexcept {
    return peer_process_id_;
  }
  [[nodiscard]] const std::wstring &last_error() const noexcept {
    return last_error_;
  }

private:
  // Tests inspect terminal I/O state at the same boundary used by destruction.
  friend struct NamedPipeEditorDaemonChannelTestAccess;
  bool create_server_pipe();
  bool connect_client();
  struct PeerSpec {
    bool server;
    const wchar_t *image;
    const wchar_t *identity_error;
    const wchar_t *unexpected_error;
    const wchar_t *session_error;
    const wchar_t *authenticate_error;
    const wchar_t *untrusted_error;
  };
  bool authenticate_peer(const PeerSpec &spec);
  bool verify_peer_identity(DWORD peer_pid, const wchar_t *sibling_image,
                            std::wstring_view session_error,
                            std::wstring_view authenticate_error,
                            std::wstring_view untrusted_error);
  void complete_connection();
  void start_read(std::vector<layerc::core::LiveMessageResult> &messages);
  void continue_write();
  // One poll step each: finish a pending overlapped I/O if its event is
  // signaled, then continue the read/write state machine. Return false after
  // a disconnect so the caller can re-arm the server once.
  bool service_connect();
  bool service_write();
  bool service_read(std::vector<layerc::core::LiveMessageResult> &messages);
  void disconnect(DWORD error);
  // Cancel and drain submitted I/O before closing the pipe or reusing storage.
  void close_handles() noexcept;
  void set_error(DWORD error);

  static constexpr std::size_t io_buffer_size = 64U * 1024U;
  static constexpr std::size_t max_messages_per_poll = 64;
  void queue_decoded(std::vector<layerc::core::LiveMessageResult> decoded,
                     std::vector<layerc::core::LiveMessageResult> &messages);
  void drain_messages(std::vector<layerc::core::LiveMessageResult> &messages);
  std::deque<layerc::core::LiveMessageResult> pending_messages_;
  Role role_;
  std::wstring pipe_name_;
  DWORD expected_peer_process_id_{};
  DWORD peer_process_id_{};
  HANDLE pipe_{INVALID_HANDLE_VALUE};
  HANDLE connect_event_{};
  HANDLE read_event_{};
  HANDLE write_event_{};
  OVERLAPPED connect_overlapped_{};
  OVERLAPPED read_overlapped_{};
  OVERLAPPED write_overlapped_{};
  std::array<std::byte, io_buffer_size> read_buffer_{};
  layerc::core::LiveFrameDecoder decoder_;
  std::deque<std::vector<std::byte>> write_queue_;
  std::vector<std::byte> active_write_;
  bool connect_pending_{};
  bool read_pending_{};
  bool write_pending_{};
  bool connected_{};
  bool connection_changed_{};
  ULONGLONG next_client_attempt_{};
  std::wstring last_error_;
};

} // namespace layerc::platform
