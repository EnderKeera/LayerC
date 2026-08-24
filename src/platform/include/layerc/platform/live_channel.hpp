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

class IEditorDaemonChannel {
public:
  virtual ~IEditorDaemonChannel() = default;
  [[nodiscard]] virtual bool connected() const noexcept = 0;
  [[nodiscard]] virtual bool send(const layerc::core::LiveMessage &message) = 0;
  [[nodiscard]] virtual std::vector<layerc::core::LiveMessageResult> poll() = 0;
};

class NamedPipeEditorDaemonChannel final : public IEditorDaemonChannel {
public:
  enum class Role { server, client };

  explicit NamedPipeEditorDaemonChannel(
      Role role, std::wstring pipe_name = live_edit_pipe_name,
      DWORD expected_peer_process_id = 0);
  ~NamedPipeEditorDaemonChannel() override;
  NamedPipeEditorDaemonChannel(const NamedPipeEditorDaemonChannel &) = delete;
  NamedPipeEditorDaemonChannel &
  operator=(const NamedPipeEditorDaemonChannel &) = delete;

  [[nodiscard]] bool connected() const noexcept override { return connected_; }
  [[nodiscard]] bool send(const layerc::core::LiveMessage &message) override;
  [[nodiscard]] std::vector<layerc::core::LiveMessageResult> poll() override;
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
  bool create_server_pipe();
  bool connect_client();
  bool authenticate_server();
  bool authenticate_client();
  bool verify_peer_identity(DWORD peer_pid, const wchar_t *sibling_image,
                            std::wstring_view session_error,
                            std::wstring_view authenticate_error,
                            std::wstring_view untrusted_error);
  void complete_connection();
  void start_read(std::vector<layerc::core::LiveMessageResult> &messages);
  void continue_write();
  void disconnect(DWORD error);
  void close_handles();
  void set_error(DWORD error);

  static constexpr std::size_t io_buffer_size = 64U * 1024U;
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
