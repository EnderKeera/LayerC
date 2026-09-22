#include "control_channel.hpp"

#include "layerc/core/frame.hpp"
#include "layerc/core/json.hpp"

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

[[nodiscard]] bool write_all(int fd, std::string_view data, int timeout_ms,
                             std::string &error) {
  // Bounded by a monotonic deadline, not a round count: each EAGAIN waits
  // only the time left (at most one poll slice), so a slow peer gets the
  // full control_write_timeout but no more.
  constexpr int poll_slice_ms = 100;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (!data.empty()) {
    const ssize_t sent = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (sent > 0) {
      data.remove_prefix(static_cast<std::size_t>(sent));
      continue;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      const auto remaining = std::chrono::duration_cast<
          std::chrono::milliseconds>(deadline -
                                     std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        error = "control peer did not accept the full frame";
        return false;
      }
      ::pollfd watch{.fd = fd, .events = POLLOUT, .revents = 0};
      (void)::poll(&watch, 1, static_cast<int>(std::min<std::int64_t>(
                                       remaining.count(), poll_slice_ms)));
      continue;
    }
    error = "control write failed";
    return false;
  }
  return true;
}

} // namespace

std::optional<ControlCommand>
parse_control_command(const std::string_view name) noexcept {
  if (name == "status") return ControlCommand::status;
  if (name == "reload") return ControlCommand::reload;
  if (name == "notify-reload") return ControlCommand::notify_reload;
  if (name == "stop") return ControlCommand::stop;
  return std::nullopt;
}

std::string encode_control_request(const ControlCommand command) {
  std::string out;
  out += "{\"proto\":";
  layerc::core::json::append_quoted(out, control_protocol_name);
  out += ",\"v\":1,\"cmd\":";
  layerc::core::json::append_quoted(out, control_command_name(command));
  out += "}";
  return out;
}

std::optional<ControlCommand> parse_control_request(std::string_view json_text) {
  const auto parsed = layerc::core::json::parse(json_text);
  const auto *value = std::get_if<layerc::core::json::Value>(&parsed);
  if (value == nullptr || !value->is_object()) return std::nullopt;
  const auto *proto = value->find("proto");
  const auto *version = value->find("v");
  const auto *command = value->find("cmd");
  if (proto == nullptr || !proto->is_string() ||
      proto->as_string() != control_protocol_name)
    return std::nullopt;
  if (version == nullptr || !version->is_integer() ||
      version->as_integer() != static_cast<std::int64_t>(control_protocol_version))
    return std::nullopt;
  if (command == nullptr || !command->is_string()) return std::nullopt;
  return parse_control_command(command->as_string());
}

std::string encode_control_response(const bool ok, std::string_view error,
                                    std::string_view payload_json) {
  std::string out;
  out += "{\"proto\":";
  layerc::core::json::append_quoted(out, control_protocol_name);
  out += ",\"v\":1,\"ok\":";
  layerc::core::json::append_bool(out, ok);
  if (!payload_json.empty()) {
    out += ",\"payload\":";
    out += payload_json;
  }
  if (!error.empty()) {
    out += ",\"error\":";
    layerc::core::json::append_quoted(out, error);
  }
  out += "}";
  return out;
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
  if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
    error = "could not create control directory";
    return std::nullopt;
  }
  struct stat status {};
  if (::lstat(directory.c_str(), &status) != 0) {
    error = "control directory is not inspectable";
    return std::nullopt;
  }
  if (S_ISLNK(status.st_mode) || !S_ISDIR(status.st_mode) ||
      status.st_uid != ::geteuid() ||
      (status.st_mode & 0777U) != 0700U) {
    error = "control directory is not owner-only";
    return std::nullopt;
  }

  ::sockaddr_un address{};
  if (!socket_addr(*path, address)) {
    error = "control socket path exceeds the unix name limit";
    return std::nullopt;
  }
  struct stat socket_status {};
  if (::lstat(path->c_str(), &socket_status) == 0) {
    // Probe before unlink: refuse while a live daemon answers, and only
    // then treat the inode as stale.
    const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe < 0) {
      error = "could not probe the control socket";
      return std::nullopt;
    }
    const bool live = ::connect(probe, reinterpret_cast<sockaddr *>(&address),
                                sizeof(address)) == 0;
    (void)::close(probe);
    if (live) {
      error = "another LayerC daemon is already serving the control socket";
      return std::nullopt;
    }
    if (::unlink(path->c_str()) != 0) {
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
    (void)::close(listener.fd_);
    error = errno == EADDRINUSE
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

std::optional<int> control_connect(std::string &error) {
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
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    error = "could not create the control client";
    return std::nullopt;
  }
  if (::connect(fd, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0) {
    (void)::close(fd);
    error = "the LayerC daemon is not reachable";
    return std::nullopt;
  }
  (void)::fcntl(fd, F_SETFL, O_NONBLOCK);
  return fd;
}

std::optional<std::string>
control_roundtrip(std::string_view request_json, std::string &error) {
  const auto fd = control_connect(error);
  if (!fd.has_value()) return std::nullopt;
  const std::string packet = frame(request_json);
  if (!write_all(*fd, packet, 5000, error)) {
    (void)::close(*fd);
    return std::nullopt;
  }
  ControlFramer framer;
  std::array<char, 4096> chunk{};
  for (int rounds = 0; rounds < 50; ++rounds) { // 5 s ceiling
    ::pollfd watch{.fd = *fd, .events = POLLIN, .revents = 0};
    if (::poll(&watch, 1, 100) > 0) {
      const ssize_t received =
          ::read(*fd, chunk.data(), chunk.size());
      if (received > 0) {
        framer.feed(std::string_view{chunk.data(),
                                     static_cast<std::size_t>(received)});
        if (auto payload = framer.next_frame(); payload.has_value()) {
          (void)::close(*fd);
          return payload;
        }
      } else if (received == 0) {
        break; // closed before a full frame
      }
    }
    if (framer.fault() != ControlFramer::Fault::none) break;
  }
  (void)::close(*fd);
  error = framer.fault() == ControlFramer::Fault::none
              ? "the daemon did not return a control response"
              : "the daemon violated the control frame limits";
  return std::nullopt;
}

bool control_send_frame(int fd, std::string_view payload, std::string &error) {
  return write_all(fd, frame(payload), 2000, error);
}

bool peer_uid_matches(const int fd) noexcept {
  ::ucred peer{};
  socklen_t length = sizeof(peer);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0)
    return false;
  return peer.uid == ::geteuid();
}

} // namespace layerc::platform
