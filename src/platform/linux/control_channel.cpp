#include "control_channel.hpp"

#include "layerc/core/frame.hpp"
#include "layerc/core/json.hpp"
#include "layerc/core/json_reader.hpp"
#include "layerc/platform/profile_file_guard_linux.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace layerc::platform {
namespace {

using layerc::core::append_frame_header;
using layerc::core::frame_header_bytes;
using layerc::core::read_frame_header;

std::string frame(std::string_view payload) {
  std::string out(frame_header_bytes, '\0');
  append_frame_header(reinterpret_cast<std::byte *>(out.data()),
                      static_cast<std::uint32_t>(payload.size()));
  out.append(payload);
  return out;
}

bool socket_addr(const std::string &path, ::sockaddr_un &address) {
  if (path.empty() || path.size() >= sizeof(address.sun_path)) return false;
  std::memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.data(), path.size());
  return true;
}

using Clock = std::chrono::steady_clock;

[[nodiscard]] bool poll_until(int fd, short events, Clock::time_point deadline) {
  while (Clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now());
    ::pollfd watch{.fd = fd, .events = events, .revents = 0};
    const int ready = ::poll(&watch, 1, static_cast<int>(
        std::max<std::int64_t>(1, std::min<std::int64_t>(remaining.count(), 100))));
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0 || (watch.revents & (POLLNVAL | POLLERR)) != 0)
      return false;
    if (ready > 0) return (watch.revents & events) != 0;
  }
  return false;
}

[[nodiscard]] bool connect_until(int fd, const ::sockaddr_un &address,
                                 Clock::time_point deadline) {
  while (Clock::now() < deadline) {
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&address),
                  sizeof(address)) == 0 || errno == EISCONN)
      return true;
    const int failure = errno;
    if (failure == EINPROGRESS || failure == EALREADY || failure == EINTR) {
      const bool ready = poll_until(fd, POLLOUT, deadline);
      int socket_error = 0;
      socklen_t length = sizeof(socket_error);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0)
        return false;
      if (socket_error != 0) {
        errno = socket_error;
        return false;
      }
      if (ready) return true;
      errno = Clock::now() >= deadline ? ETIMEDOUT : ECONNABORTED;
      return false;
    }
    if (failure != EAGAIN && failure != EWOULDBLOCK) return false;
    // A full AF_UNIX listen queue reports EAGAIN without beginning a
    // connection. POLLOUT would be immediately ready: sleep before retrying
    // instead of spinning, while retaining the same monotonic deadline.
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now());
    const int pause_ms = static_cast<int>(
        std::clamp<std::int64_t>(remaining.count(), 1, 10));
    if (::poll(nullptr, 0, pause_ms) < 0 && errno != EINTR) return false;
  }
  errno = ETIMEDOUT;
  return false;
}

[[nodiscard]] bool write_all(int fd, std::string_view data,
                             Clock::time_point deadline, std::string &error) {
  while (!data.empty()) {
    if (Clock::now() >= deadline) {
      error = "control peer did not accept the full frame";
      return false;
    }
    const ssize_t sent = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (sent > 0) {
      data.remove_prefix(static_cast<std::size_t>(sent));
      continue;
    }
    if (sent < 0 && errno == EINTR) continue;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (poll_until(fd, POLLOUT, deadline)) continue;
      error = "control peer did not accept the full frame";
      return false;
    }
    error = "control write failed";
    return false;
  }
  return true;
}

} // namespace

std::string encode_control_request(const ControlCommand command,
                                   const std::string_view payload_json) {
  std::string out;
  out += "{\"proto\":";
  layerc::core::json::append_quoted(out, control_protocol_name);
  out += ",\"v\":1,\"cmd\":";
  layerc::core::json::append_quoted(out, control_command_name(command));
  if (!payload_json.empty()) { out += ",\"payload\":"; out += payload_json; }
  out += "}";
  return out;
}

namespace {

// Requests and responses share the LayerC.Control v1 envelope.
[[nodiscard]] bool has_control_envelope(const layerc::core::json::Value &value) {
  if (!value.is_object()) return false;
  const auto *proto = value.find("proto");
  const auto *version = value.find("v");
  return proto != nullptr && proto->is_string() &&
         proto->as_string() == control_protocol_name && version != nullptr &&
         version->is_integer() &&
         version->as_integer() ==
             static_cast<std::int64_t>(control_protocol_version);
}

} // namespace

std::optional<ControlRequest> parse_control_request_envelope(const std::string_view json_text) {
  auto parsed = layerc::core::json::parse_bounded(json_text, control_frame_limit);
  auto *value = std::get_if<layerc::core::json::Value>(&parsed);
  if (value == nullptr || !has_control_envelope(*value)) return std::nullopt;
  const auto *command = value->find("cmd");
  if (command == nullptr || !command->is_string()) return std::nullopt;
  const auto parsed_command = parse_control_command(command->as_string());
  if (!parsed_command.has_value()) return std::nullopt;
  ControlRequest result{*parsed_command, std::nullopt};
  if (auto *payload = value->find("payload")) result.payload = *payload;
  return result;
}

std::optional<ControlCommand> parse_control_request(std::string_view json_text) {
  const auto request = parse_control_request_envelope(json_text);
  return request.has_value() ? std::optional{request->command} : std::nullopt;
}

std::string encode_control_response(const bool ok, std::string_view error,
                                    std::string_view payload_json,
                                    std::string_view code) {
  std::string out;
  out += "{\"proto\":";
  layerc::core::json::append_quoted(out, control_protocol_name);
  out += ",\"v\":1,\"ok\":";
  layerc::core::json::append_bool(out, ok);
  if (!payload_json.empty()) {
    out += ",\"payload\":";
    out += payload_json;
  }
  if (!ok && !code.empty()) {
    out += ",\"code\":";
    layerc::core::json::append_quoted(out, code);
  }
  if (!error.empty()) {
    out += ",\"error\":";
    layerc::core::json::append_quoted(out, error);
  }
  out += "}";
  return out;
}

static std::optional<ControlCall> decode_control_call(std::string_view text) {
  const auto parsed = layerc::core::json::parse_bounded(text, control_frame_limit);
  const auto *value = std::get_if<layerc::core::json::Value>(&parsed);
  if (!value || !has_control_envelope(*value)) return std::nullopt;
  const auto *ok = value->find("ok");
  if (!ok || !ok->is_bool()) return std::nullopt;
  ControlCall call;
  call.ok = ok->as_bool();
  if (const auto *error = value->find("error")) {
    if (!error->is_string()) return std::nullopt;
    call.error = error->as_string();
  }
  if (const auto *code = value->find("code")) {
    if (!code->is_string()) return std::nullopt;
    call.code = code->as_string();
  }
  if (const auto *payload = value->find("payload")) call.payload = *payload;
  return call;
}

std::optional<ControlResponse>
decode_control_response(const std::string_view json_text) {
  auto call = decode_control_call(json_text);
  if (!call.has_value()) return std::nullopt;
  return ControlResponse{.ok = call->ok, .error = std::move(call->error),
                         .code = std::move(call->code)};
}

std::optional<std::string> control_socket_path(std::string &warning) {
  const char *const runtime_dir = std::getenv("XDG_RUNTIME_DIR");
  if (runtime_dir != nullptr && runtime_dir[0] == '/')
    return std::string(runtime_dir) + "/layerc/control.sock";
  warning =
      "layerc: XDG_RUNTIME_DIR is unset or not absolute; using the "
      "/tmp/layerc-<uid> fallback (owner-only, checked before use)";
  return "/tmp/layerc-" + std::to_string(static_cast<long>(::geteuid())) +
         "/control.sock";
}

ControlListener::~ControlListener() { close(); }

ControlListener::ControlListener(ControlListener &&other) noexcept
    : fd_(std::exchange(other.fd_, -1)), path_(std::move(other.path_)) {}

ControlListener &
ControlListener::operator=(ControlListener &&other) noexcept {
  if (this != &other) {
    close();
    fd_ = std::exchange(other.fd_, -1);
    path_ = std::move(other.path_);
  }
  return *this;
}

void ControlListener::close() noexcept {
  if (fd_ >= 0) {
    (void)::close(fd_);
    fd_ = -1;
  }
  if (!path_.empty()) {
    (void)::unlink(path_.c_str());
    path_.clear();
  }
}

std::optional<ControlListener> ControlListener::open(std::string &error,
                                                     std::string &warning) {
  const auto path = control_socket_path(warning);
  if (!path.has_value()) {
    error = "no usable control socket path";
    return std::nullopt;
  }
  const std::string directory = path->substr(0, path->find_last_of('/'));

  // Owner-only runtime directory; never accept a symlink or a directory
  // owned by somebody else, and surface the fallback decision.
  if (!ensure_owned_directory(directory,
                              OwnedDirectoryPolicy::require_private)) {
    error = "control directory is not owner-only";
    return std::nullopt;
  }

  ::sockaddr_un address{};
  if (!socket_addr(*path, address)) {
    error = "control socket path exceeds the unix name limit";
    return std::nullopt;
  }
  struct stat socket_status {};
  const int path_status = ::lstat(path->c_str(), &socket_status);
  if (path_status != 0 && errno != ENOENT) {
    error = "could not inspect the control socket";
    return std::nullopt;
  }
  if (path_status == 0) {
    if (!S_ISSOCK(socket_status.st_mode) || socket_status.st_uid != ::geteuid()) {
      error = "the control socket path is not an owned socket";
      return std::nullopt;
    }
    // Probe before unlink: refuse while a live daemon answers, and only
    // then treat the inode as stale.
    const int probe =
        ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (probe < 0) {
      error = "could not probe the control socket";
      return std::nullopt;
    }
    const bool live =
        connect_until(probe, address, Clock::now() + std::chrono::seconds(1));
    const int probe_error = live ? 0 : errno;
    (void)::close(probe);
    if (live) {
      error = "another LayerC daemon is already serving the control socket";
      return std::nullopt;
    }
    if (probe_error != ECONNREFUSED && probe_error != ENOENT) {
      error = "could not prove the control socket stale; leaving it intact";
      return std::nullopt;
    }
    if (::unlink(path->c_str()) != 0 && errno != ENOENT) {
      error = "could not remove the stale control socket";
      return std::nullopt;
    }
  }

  ControlListener listener;
  listener.fd_ =
      ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listener.fd_ < 0) {
    error = "could not create the control listener";
    return std::nullopt;
  }
  if (::bind(listener.fd_, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) != 0) {
    const int bind_error = errno;
    listener.close();
    error = bind_error == EADDRINUSE
                ? "the control socket appeared during startup"
                : "could not bind the control socket";
    return std::nullopt;
  }
  listener.path_ = *path;
  if (::chmod(listener.path_.c_str(), 0600) != 0 ||
      ::listen(listener.fd_, 8) != 0) {
    error = "could not protect the control socket";
    listener.close();
    return std::nullopt;
  }
  return listener;
}

ControlAcceptResult ControlListener::accept() {
  while (true) {
    const int client =
        ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return {};
      return {ControlAcceptResult::State::rejected, -1,
              "control accept failed"};
    }
    if (!peer_uid_matches(client)) {
      (void)::close(client);
      return {ControlAcceptResult::State::rejected, -1,
              "rejected a control peer with a different effective uid"};
    }
    return {ControlAcceptResult::State::accepted, client, {}};
  }
}

std::optional<int> control_connect(std::string &error,
                                   const Clock::time_point deadline) {
  std::string warning;
  const auto path = control_socket_path(warning);
  if (!path.has_value()) {
    error = "no usable control socket path";
    return std::nullopt;
  }
  ::sockaddr_un address{};
  if (!socket_addr(*path, address)) {
    error = "control socket path exceeds the unix name limit";
    return std::nullopt;
  }
  const int fd =
      ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    error = "could not create the control client";
    return std::nullopt;
  }
  if (!connect_until(fd, address, deadline)) {
    const int failure = errno;
    (void)::close(fd);
    error = failure == ETIMEDOUT
        ? "control connection timed out"
        : "the LayerC daemon is not reachable: " + std::string(std::strerror(failure));
    return std::nullopt;
  }
  if (!peer_uid_matches(fd)) {
    (void)::close(fd);
    error = "rejected a control server with a different effective uid";
    return std::nullopt;
  }
  return fd;
}

static std::optional<std::string>
control_exchange_raw(std::string_view request_json, std::string &error,
                     const std::chrono::milliseconds timeout) {
  if (request_json.size() > control_frame_limit || timeout <= std::chrono::milliseconds::zero() ||
      timeout > std::chrono::seconds(5)) {
    error = "invalid control request size or timeout";
    return std::nullopt;
  }
  const auto deadline = Clock::now() + timeout;
  const auto fd = control_connect(error, deadline);
  if (!fd.has_value()) return std::nullopt;
  const std::string packet = frame(request_json);
  if (!write_all(*fd, packet, deadline, error)) {
    (void)::close(*fd);
    return std::nullopt;
  }
  layerc::core::FrameDecoder framer{control_frame_limit, control_buffer_limit};
  std::array<char, 4096> chunk{};
  while (poll_until(*fd, POLLIN, deadline)) {
    const ssize_t received = ::read(*fd, chunk.data(), chunk.size());
    if (received < 0 && (errno == EINTR || errno == EAGAIN ||
                         errno == EWOULDBLOCK))
      continue;
    if (received <= 0) break; // closed or failed before a full frame
    framer.feed(layerc::core::text_bytes(std::string_view{
        chunk.data(), static_cast<std::size_t>(received)}));
    if (auto payload = framer.next_frame(); payload.has_value()) {
      (void)::close(*fd);
      return payload;
    }
    if (framer.fault() != layerc::core::FrameDecoder::Fault::none) break;
  }
  (void)::close(*fd);
  error = framer.fault() == layerc::core::FrameDecoder::Fault::none
              ? "the daemon did not return a control response"
              : "the daemon violated the control frame limits";
  return std::nullopt;
}

std::optional<std::string>
control_roundtrip(std::string_view request_json, std::string &error,
                  const std::chrono::milliseconds timeout) {
  const auto raw = control_exchange_raw(request_json, error, timeout);
  if (!raw.has_value()) return std::nullopt;
  const auto call = decode_control_call(*raw);
  if (!call.has_value() || !call->ok) {
    error = call.has_value() && !call->error.empty()
                ? call->error
                : "the daemon returned an invalid response";
    return std::nullopt;
  }
  return raw; // Preserve accepted response bytes for JSON presentation.
}

ControlCall control_call(const ControlCommand command, std::string_view payload_json,
                         const std::chrono::milliseconds timeout) {
  std::string error;
  const auto raw = control_exchange_raw(encode_control_request(command, payload_json), error,
                                        timeout);
  if (!raw.has_value())
    return {.ok = false, .code = {}, .error = std::move(error), .payload = std::nullopt};
  auto call = decode_control_call(*raw);
  if (!call.has_value())
    return {.ok = false, .code = {}, .error = "the daemon returned an invalid response",
            .payload = std::nullopt};
  if (!call->ok && call->error.empty())
    call->error = "the daemon returned an invalid response";
  return std::move(*call);
}

bool control_send_frame(int fd, std::string_view payload, std::string &error) {
  return write_all(fd, frame(payload), Clock::now() + std::chrono::seconds(2), error);
}

bool peer_uid_matches(const int fd) noexcept {
  ::ucred peer{};
  socklen_t length = sizeof(peer);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0)
    return false;
  return peer.uid == ::geteuid();
}

} // namespace layerc::platform
