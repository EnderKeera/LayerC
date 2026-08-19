#pragma once

#include "layerc/core/companion.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace layerc::gamebar {

// Local publication endpoint used by the packaged companion. The daemon owns
// the pipe and only publishes immutable, bounded snapshot JSON; the companion
// never receives authority over cooldown state.
class GameBarTransport final {
public:
  GameBarTransport() = default;
  ~GameBarTransport();
  GameBarTransport(const GameBarTransport &) = delete;
  GameBarTransport &operator=(const GameBarTransport &) = delete;

  [[nodiscard]] bool start();
  void stop() noexcept;
  void poll() noexcept;
  [[nodiscard]] bool publish(std::string_view snapshot_json);
  [[nodiscard]] bool connected() const noexcept { return connected_; }
  [[nodiscard]] HANDLE wait_handle() const noexcept;
  [[nodiscard]] const std::wstring &last_error() const noexcept {
    return last_error_;
  }
  [[nodiscard]] std::size_t resource_handle_count() const noexcept;

  inline static constexpr wchar_t pipe_name[] =
      L"\\\\.\\pipe\\LayerC.GameBar.v1";
  inline static constexpr std::wstring_view package_name =
      L"LayerC.GameBar.v1";

private:
  [[nodiscard]] bool create_server();
  [[nodiscard]] bool authenticate_client();
  void complete_connection() noexcept;
  void disconnect() noexcept;
  [[nodiscard]] bool continue_write();
  void set_error(DWORD error);

  static constexpr std::size_t max_frame_bytes =
      layerc::core::max_companion_snapshot_bytes;
  HANDLE pipe_{INVALID_HANDLE_VALUE};
  HANDLE connect_event_{};
  HANDLE write_event_{};
  OVERLAPPED connect_overlapped_{};
  OVERLAPPED write_overlapped_{};
  bool started_{};
  bool connect_pending_{};
  bool write_pending_{};
  bool connected_{};
  std::vector<std::byte> pending_frame_;
  std::vector<std::byte> active_frame_;
  std::wstring last_error_;
};

} // namespace layerc::gamebar
