#pragma once

#include "layerc/core/enum_wire.hpp"
#include "layerc/core/frame.hpp"
#include "layerc/core/utf8.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::platform {

// LayerC.Control v1 request/response channel over one filesystem-scoped
// AF_UNIX SOCK_STREAM socket. Framing reuses the shared 4-byte little-endian
// length prefix (core/frame.hpp) around UTF-8 JSON; one request and one
// response per connection. Distinct from the editor's LayerE protocol.
// Readers decode with core::FrameDecoder{control_frame_limit,
// control_buffer_limit}; oversized frames or buffers fault stickily.
inline constexpr std::string_view control_protocol_name = "LayerC.Control";
inline constexpr std::uint32_t control_protocol_version = 1;
inline constexpr std::uint32_t control_frame_limit = 512U * 1024U;
inline constexpr std::size_t control_buffer_limit = 4U * 1024U * 1024U;

enum class ControlCommand : std::uint8_t {
  status,
  reload,
  notify_reload,
  stop,
  trace,
};

inline constexpr std::array control_command_wire{
    layerc::core::EnumWireEntry{ControlCommand::status, "status"},
    layerc::core::EnumWireEntry{ControlCommand::reload, "reload"},
    layerc::core::EnumWireEntry{ControlCommand::notify_reload, "notify-reload"},
    layerc::core::EnumWireEntry{ControlCommand::stop, "stop"},
    layerc::core::EnumWireEntry{ControlCommand::trace, "trace"},
};

[[nodiscard]] constexpr std::string_view
control_command_name(const ControlCommand command) noexcept {
  return layerc::core::enum_wire_name(control_command_wire, command)
      .value_or(std::string_view{});
}

[[nodiscard]] constexpr std::optional<ControlCommand>
parse_control_command(const std::string_view name) noexcept {
  return layerc::core::parse_enum_wire(control_command_wire, name);
}

// Request JSON: {"proto":"LayerC.Control","v":1,"cmd":"<command>"}
// Response JSON: {"proto":"LayerC.Control","v":1,"ok":true[,"payload":<json>]
//                 [,"error":"<text>"]}
[[nodiscard]] std::string encode_control_request(ControlCommand command,
                                                 std::string_view payload_json = {});

[[nodiscard]] std::optional<ControlCommand>
parse_control_request(std::string_view json);

[[nodiscard]] std::string encode_control_response(bool ok,
                                                  std::string_view error,
                                                  std::string_view payload_json);

struct ControlResponse {
  bool ok{};
  std::string error;
};

// Mirror of encode_control_response; nullopt for anything that is not a
// LayerC.Control v1 response envelope.
[[nodiscard]] std::optional<ControlResponse>
decode_control_response(std::string_view json);

// Resolves the control socket path without touching the filesystem:
// $XDG_RUNTIME_DIR/layerc/control.sock, falling back to
// /tmp/layerc-<uid>/control.sock (warning appended for the fallback).
// An empty result means no usable root (e.g. absurd XDG_RUNTIME_DIR).
[[nodiscard]] std::optional<std::string>
control_socket_path(std::string &warning);

// Server side: verifies/creates the runtime directory owner-only, probes a
// stale socket before unlinking, refuses to bind while another daemon
// answers, and enforces socket mode 0600.
struct ControlAcceptResult {
  enum class State : std::uint8_t { none, accepted, rejected };

  State state{State::none};
  int fd{-1};
  std::string error;
};

class ControlListener {
public:
  ControlListener() = default;
  ~ControlListener();
  ControlListener(ControlListener &&other) noexcept;
  ControlListener &operator=(ControlListener &&other) noexcept;
  ControlListener(const ControlListener &) = delete;

  [[nodiscard]] static std::optional<ControlListener>
  open(std::string &error, std::string &warning);

  [[nodiscard]] int fd() const noexcept { return fd_; }
  // Accepts one client and enforces the effective-UID peer check.
  [[nodiscard]] ControlAcceptResult accept();

private:
  void close() noexcept;

  int fd_{-1};
  std::string path_;
};

// Client side: connects to the resolved socket path (no directory
// creation — offline commands must not invent runtime dirs).
// Authenticates the server's effective UID before returning a connected fd;
// Nonblocking establishment is bounded by a monotonic deadline; nullopt
// with error text when no same-UID daemon answers.
[[nodiscard]] std::optional<int> control_connect(
    std::string &error, std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5));

// Blocking CLI/store roundtrip: one five-second monotonic deadline covers
// connection, frame write, and response read. Accepts only a valid, successful
// LayerC.Control v1 response; returns its original bytes for presentation.
[[nodiscard]] std::optional<std::string>
control_roundtrip(std::string_view request_json, std::string &error,
                  std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Blocking framed write with a bounded POLLOUT wait (CLI/test helpers only;
// daemon responses advance nonblocking writes on the shared event loop).
[[nodiscard]] bool control_send_frame(int fd, std::string_view payload,
                                      std::string &error);

[[nodiscard]] bool peer_uid_matches(int fd) noexcept;

} // namespace layerc::platform
