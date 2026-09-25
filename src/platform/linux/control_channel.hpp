#pragma once

#include "layerc/core/frame.hpp"

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
inline constexpr std::string_view control_protocol_name = "LayerC.Control";
inline constexpr std::uint32_t control_protocol_version = 1;
inline constexpr std::uint32_t control_frame_limit = 512U * 1024U;
inline constexpr std::size_t control_buffer_limit = 4U * 1024U * 1024U;

enum class ControlCommand : std::uint8_t {
  status,
  reload,
  notify_reload,
  stop,
};

[[nodiscard]] constexpr std::string_view
control_command_name(ControlCommand command) noexcept {
  switch (command) {
  case ControlCommand::status: return "status";
  case ControlCommand::reload: return "reload";
  case ControlCommand::notify_reload: return "notify-reload";
  case ControlCommand::stop: return "stop";
  }
  return {};
}

[[nodiscard]] std::optional<ControlCommand>
parse_control_command(std::string_view name) noexcept;

// Request JSON: {"proto":"LayerC.Control","v":1,"cmd":"<command>"}
// Response JSON: {"proto":"LayerC.Control","v":1,"ok":true[,"payload":<json>]
//                 [,"error":"<text>"]}
[[nodiscard]] std::string encode_control_request(ControlCommand command);

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

// Incremental byte-stream → frame decoder with the plan's bounds: partial
// frames accumulate, an oversized declared frame or buffer is sticky-faulted.
class ControlFramer {
public:
  using Fault = layerc::core::FrameDecoder::Fault;

  void feed(std::string_view bytes) {
    decoder_.feed(std::as_bytes(std::span(bytes.data(), bytes.size())));
  }
  [[nodiscard]] std::optional<std::string> next_frame() {
    return decoder_.next_frame();
  }
  [[nodiscard]] Fault fault() const noexcept { return decoder_.fault(); }

private:
  layerc::core::FrameDecoder decoder_{control_frame_limit, control_buffer_limit};
};

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
// Returns a connected fd, or nullopt with error text when no daemon
// answers.
[[nodiscard]] std::optional<int> control_connect(std::string &error);

// Blocking request/response roundtrip for the CLI: sends one frame, then
// polls for exactly one response frame with a bounded wait.
[[nodiscard]] std::optional<std::string>
control_roundtrip(std::string_view request_json, std::string &error);

// Framed write with a bounded POLLOUT wait (daemon response path).
[[nodiscard]] bool control_send_frame(int fd, std::string_view payload,
                                      std::string &error);

[[nodiscard]] bool peer_uid_matches(int fd) noexcept;

} // namespace layerc::platform
