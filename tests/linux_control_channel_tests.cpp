#include "control_channel.hpp"
#include "daemon_host.hpp"
#include "event_loop.hpp"

#include "layerc/core/json.hpp"
#include "layerc/runtime/daemon_kernel.hpp"
#include "test_support.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using layerc::platform::ControlFramer;
using layerc::platform::ControlListener;

namespace control = layerc::platform;

std::string make_frame(std::string_view payload) {
  std::string framed;
  framed.push_back(static_cast<char>(payload.size() & 0xFF));
  framed.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
  framed.push_back(static_cast<char>((payload.size() >> 16) & 0xFF));
  framed.push_back(static_cast<char>((payload.size() >> 24) & 0xFF));
  framed += payload;
  return framed;
}

} // namespace

void control_framer_contracts() {
  using layerc::test::expect;
  bool passed = true;

  // Framer: partial bytes, multi-frame coalescing, and sticky bounds faults.
  {
    const std::string request = control::encode_control_request(
        control::ControlCommand::status);
    const std::string framed = make_frame(request);

    ControlFramer framer;
    for (std::size_t index = 0; index + 1 < framed.size(); ++index)
      framer.feed(std::string_view{framed}.substr(index, 1));
    passed &= expect(!framer.next_frame().has_value() &&
                         framer.fault() == ControlFramer::Fault::none,
                     "partial frames must not decode");
    framer.feed(std::string_view{framed}.substr(framed.size() - 1));
    passed &= expect(framer.next_frame() == request &&
                         !framer.next_frame().has_value(),
                     "the final byte should complete exactly one frame");

    ControlFramer batched;
    batched.feed(framed + framed);
    passed &= expect(batched.next_frame() == request &&
                         batched.next_frame() == request &&
                         !batched.next_frame().has_value(),
                     "coalesced frames must decode independently");

    ControlFramer oversized;
    oversized.feed(std::string("\xFF\xFF\xFF\xFF", 4));
    (void)oversized.next_frame();
    passed &= expect(oversized.fault() == ControlFramer::Fault::frame_too_large,
                     "declared frames above 512 KiB must fault");

    ControlFramer flooded;
    flooded.feed(std::string(4 * 1024 * 1024 + 8, 'x'));
    passed &= expect(flooded.fault() == ControlFramer::Fault::buffer_too_large,
                     "undrained buffers above 4 MiB must fault");
  }

  (void)passed;
}

void control_protocol_contracts() {
  using layerc::test::expect;
  bool passed = true;
  // Protocol codec: strict LayerC.Control v1 envelope.
  {
    const auto request = control::encode_control_request(
        control::ControlCommand::reload);
    passed &= expect(control::parse_control_request(request) ==
                         std::optional<control::ControlCommand>{
                             control::ControlCommand::reload},
                     "encoded requests must roundtrip");
    passed &= expect(!control::parse_control_request(
                         R"({"proto":"LayerE","v":1,"cmd":"reload"})")
                          .has_value(),
                     "foreign protocols must be rejected");
    passed &= expect(!control::parse_control_request(
                         R"({"proto":"LayerC.Control","v":2,"cmd":"x"})")
                          .has_value(),
                     "unknown versions must be rejected");
    passed &= expect(!control::parse_control_request("not json").has_value(),
                     "non-JSON requests must be rejected");

    const auto response = control::encode_control_response(
        true, {}, R"({"pid":42})");
    const auto parsed = layerc::core::json::parse(response);
    const auto *value = std::get_if<layerc::core::json::Value>(&parsed);
    passed &= expect(value != nullptr &&
                         value->find("ok") != nullptr &&
                         value->find("ok")->as_bool() &&
                         value->find("payload") != nullptr &&
                         value->find("payload")->find("pid") != nullptr,
                     "responses must carry ok + payload");
    const auto decoded = control::decode_control_response(response);
    passed &= expect(decoded.has_value() && decoded->ok &&
                         decoded->error.empty(),
                     "decode mirrors an encoded success response");
    const auto decoded_failure = control::decode_control_response(
        control::encode_control_response(false, "nope", {}));
    passed &= expect(decoded_failure.has_value() && !decoded_failure->ok &&
                         decoded_failure->error == "nope",
                     "decode mirrors an encoded failure and its error text");
    passed &= expect(
        !control::decode_control_response(R"({"ok":true})").has_value() &&
            !control::decode_control_response(
                 R"({"proto":"LayerC.Control","v":2,"ok":true})")
                 .has_value() &&
            !control::decode_control_response("not json").has_value(),
        "decode rejects responses outside the v1 envelope");
    const auto failure =
        control::encode_control_response(false, "nope", {});
    const auto parsed_failure = layerc::core::json::parse(failure);
    const auto *failure_value =
        std::get_if<layerc::core::json::Value>(&parsed_failure);
    passed &= expect(failure_value != nullptr &&
                         !failure_value->find("ok")->as_bool() &&
                         failure_value->find("error") != nullptr &&
                         failure_value->find("payload") == nullptr,
                     "error responses must carry ok=false + error");
  }

  (void)passed;
}

void control_dispatch_contracts() {
  using layerc::test::expect;
  bool passed = true;
  // Daemon dispatch: status/reload ack, stop flag, and rejection paths.
  {
    layerc::runtime::DaemonKernel kernel;
    (void)kernel.reconcile(kernel.demand());
    layerc::daemon::LinuxStatusSnapshot linux_status;
    linux_status.x11 = {
        true,
        {true, 2, 4},
        {false, 0, 0},
        {true, 1, 6},
        {true, 0, 4},
        {true, 1, 1},
        {true, 6, 0},
        true,
        {}};
    linux_status.capabilities = {
        {layerc::runtime::Capability::overlay,
         layerc::daemon::CapabilityAvailability::unavailable,
         {"extension_missing"}},
    };
    const auto status = layerc::daemon::handle_control_request(
        control::encode_control_request(control::ControlCommand::status),
        kernel, linux_status);
    passed &= expect(status.response.find("\"ok\":true") != std::string::npos &&
                         status.response.find("\"pid\"") != std::string::npos &&
                         status.response.find("\"resources\"") != std::string::npos &&
                         status.response.find("\"x11\":{\"connected\":true") !=
                             std::string::npos &&
                         status.response.find("\"major\":2") != std::string::npos &&
                         status.response.find("\"composite\":{\"available\":true") !=
                             std::string::npos &&
                         status.response.find("\"damage\":{\"available\":true") !=
                             std::string::npos &&
                         status.response.find("\"xfixes\":{\"available\":true") !=
                             std::string::npos &&
                         status.response.find("\"availability\":\"unavailable\"") !=
                             std::string::npos &&
                         status.response.find("\"extension_missing\"") !=
                             std::string::npos,
                     "status must return capability and X11 versions");
    const auto reload = layerc::daemon::handle_control_request(
        control::encode_control_request(control::ControlCommand::reload),
        kernel);
    passed &= expect(reload.response.find("\"ok\":true") != std::string::npos &&
                         !reload.stop_requested && reload.reload_requested,
                     "reload must acknowledge, request a reload and not stop");
    const auto stop = layerc::daemon::handle_control_request(
        control::encode_control_request(control::ControlCommand::stop), kernel);
    passed &= expect(stop.response.find("\"ok\":true") != std::string::npos &&
                         stop.stop_requested && !stop.reload_requested,
                     "stop must acknowledge and request shutdown");
    const auto unknown = layerc::daemon::handle_control_request(
        R"({"proto":"LayerC.Control","v":1,"cmd":"launch-missiles"})",
        kernel);
    passed &= expect(unknown.response.find("\"ok\":false") != std::string::npos,
                     "unknown commands must be rejected");
    const auto garbage = layerc::daemon::handle_control_request(
        "hello", kernel);
    passed &= expect(garbage.response.find("invalid LayerC.Control") !=
                         std::string::npos,
                     "malformed requests must be rejected");
  }

  (void)passed;
}

void control_socket_contracts() {
  using layerc::test::expect;
  bool passed = true;
  const layerc::test::TempDirectory directory("layerc-control");
  const auto root = directory.path();
  std::filesystem::create_directories(root / "runtime");
  const layerc::test::ScopedEnvironment runtime("XDG_RUNTIME_DIR", root / "runtime");
  if (!expect(runtime.applied(), "control-channel test runtime should be set"))
    return;

  // Socket path resolution: XDG first, owner-checked /tmp fallback.
  {
    std::string warning;
    const auto path = control::control_socket_path(warning);
    passed &= expect(path.has_value() &&
                         path->ends_with("/layerc/control.sock") &&
                         warning.empty(),
                     "XDG_RUNTIME_DIR should yield the layerc control socket");
    {
      const layerc::test::ScopedEnvironment unset_runtime("XDG_RUNTIME_DIR",
                                                         std::nullopt);
      passed &= expect(unset_runtime.applied(), "runtime should be cleared");
      warning.clear();
      const auto fallback = control::control_socket_path(warning);
      const std::string expected =
          "/tmp/layerc-" + std::to_string(static_cast<long>(::geteuid())) +
          "/control.sock";
      passed &= expect(fallback == std::optional<std::string>{expected} &&
                           !warning.empty(),
                       "missing XDG_RUNTIME_DIR should warn about the "
                       "/tmp/layerc-<uid> fallback");
    }
  }

  // Live listener: busy rejection, accepted peer, full request/response.
  {
    std::string error;
    std::string warning;
    auto listener = ControlListener::open(error, warning);
    passed &= expect(listener.has_value() && error.empty(),
                     "the control listener should open under XDG_RUNTIME_DIR");
    if (listener.has_value()) {
      struct stat status {};
      std::string socket_path;
      const auto resolved = control::control_socket_path(socket_path);
      passed &= expect(
          resolved.has_value() &&
              ::lstat(resolved->c_str(), &status) == 0 &&
              (status.st_mode & 0777U) == 0600U &&
              status.st_uid == ::geteuid(),
          "the control socket must be owner-only 0600");

      std::string second_error;
      std::string second_warning;

      const auto client = control::control_connect(error);
      passed &= expect(client.has_value(), "client should connect");
      if (client.has_value()) {
        passed &= expect(control::peer_uid_matches(*client),
                         "same-uid peers must match");
        passed &= expect(control::control_send_frame(
                             *client,
                             control::encode_control_request(
                                 control::ControlCommand::status),
                             error),
                         "client frame write should succeed");
        const auto accepted = listener->accept();
        const std::optional<int> server_side =
            accepted.state == control::ControlAcceptResult::State::accepted
                ? std::optional<int>{accepted.fd}
                : std::nullopt;
        passed &= expect(server_side.has_value() && accepted.error.empty(),
                         "listener should accept the client");
        if (server_side.has_value()) {
          ControlFramer server_framer;
          std::string request_json;
          char chunk[4096];
          while (request_json.empty()) {
            const ssize_t received =
                ::read(*server_side, chunk, sizeof(chunk));
            if (received <= 0) break;
            server_framer.feed(
                std::string_view{chunk, static_cast<std::size_t>(received)});
            request_json = server_framer.next_frame().value_or(std::string{});
          }
          layerc::runtime::DaemonKernel kernel;
          (void)kernel.reconcile(kernel.demand());
          const auto response = layerc::daemon::handle_control_request(
              request_json, kernel);
          passed &= expect(!request_json.empty() &&
                               control::control_send_frame(*server_side,
                                                           response.response,
                                                           error),
                           "the daemon should answer the request");

          ControlFramer client_framer;
          std::string response_json;
          while (response_json.empty()) {
            const ssize_t received =
                ::read(*client, chunk, sizeof(chunk));
            if (received <= 0) break;
            client_framer.feed(
                std::string_view{chunk, static_cast<std::size_t>(received)});
            response_json = client_framer.next_frame().value_or(std::string{});
          }
          passed &= expect(response_json.find("\"ok\":true") !=
                                   std::string::npos,
                           "the client should read the framed response");
          (void)::close(*server_side);
        }
        (void)::close(*client);
      }

      // After the roundtrip so its probe connection cannot steal accept().
      {
        auto second = ControlListener::open(second_error, second_warning);
        passed &= expect(!second.has_value() &&
                             second_error.find("already serving") !=
                                 std::string::npos,
                         "a second live listener must be refused");
      }
    }
  }

  // Service bounds: stalled clients are capped and expire on the monotonic
  // deadline used by the daemon event loop.
  {
    layerc::platform::EventLoop loop;
    layerc::daemon::ControlService service;
    std::string start_error;
    std::string start_warning;
    passed &= expect(loop.valid() &&
                         service.start(start_error, start_warning),
                     "bounded control service should start");
    service.attach(loop);
    layerc::runtime::DaemonKernel kernel;
    std::vector<int> peers;
    std::string diagnostic;
    for (std::size_t index = 0;
         index < layerc::daemon::control_client_limit + 1; ++index) {
      std::string connect_error;
      const auto peer = control::control_connect(connect_error);
      passed &= expect(peer.has_value(), "stalled control peer should connect");
      if (!peer.has_value()) break;
      peers.push_back(*peer);
      const auto outcome =
          service.service(loop, {service.listener_fd()}, kernel);
      if (!outcome.diagnostic.empty())
        diagnostic = outcome.diagnostic;
    }
    passed &= expect(
        service.client_count() == layerc::daemon::control_client_limit &&
            diagnostic.find("limit") != std::string::npos &&
            service.next_deadline().has_value(),
        "the service caps aggregate stalled clients and arms expiry");
    service.expire_clients(
        loop, layerc::core::MonotonicClock::now() +
                  layerc::daemon::control_client_timeout +
                  layerc::core::Milliseconds{1});
    passed &= expect(service.client_count() == 0 &&
                         !service.next_deadline().has_value(),
                     "incomplete control requests expire at their deadline");
    for (const int peer : peers) (void)::close(peer);
    service.shutdown(loop);
  }

  // Stale sockets: a dead inode is probed (refused), unlinked, and rebound.
  {
    std::string warning;
    const auto path = control::control_socket_path(warning);
    passed &= expect(path.has_value(), "control path should resolve");
    if (path.has_value()) {
      const int dead = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      ::sockaddr_un address{};
      address.sun_family = AF_UNIX;
      std::strncpy(address.sun_path, path->c_str(),
                   sizeof(address.sun_path) - 1);
      const bool bound = ::bind(dead, reinterpret_cast<sockaddr *>(&address),
                                sizeof(address)) == 0;
      (void)::close(dead); // leaves a stale socket inode behind
      passed &= expect(bound, "stale socket fixture should bind");
      std::string open_error;
      auto listener = ControlListener::open(open_error, warning);
      struct stat socket_status {};
      passed &= expect(listener.has_value() &&
                           ::lstat(path->c_str(), &socket_status) == 0,
                       "open should probe, unlink, and rebind over a stale "
                       "socket");
      listener.reset();
    }
  }

  // Hostile parent directory: a pre-created non-owner-only socket directory
  // (the exact unit the fallback protects) is refused.
  {
    const auto hostile = root / "hostile";
    std::filesystem::create_directories(hostile / "layerc");
    (void)::chmod((hostile / "layerc").c_str(), 0755);
    const layerc::test::ScopedEnvironment hostile_runtime("XDG_RUNTIME_DIR",
                                                            hostile);
    passed &= expect(hostile_runtime.applied(), "hostile runtime should be set");
    std::string error;
    std::string warning;
    passed &= expect(
        !ControlListener::open(error, warning).has_value() &&
            error.find("owner-only") != std::string::npos,
        "a non-owner-only runtime directory must be refused");
  }

  (void)passed;
}

LAYERC_TEST_MAIN(
    LAYERC_TEST_CASE(control_framer_contracts),
    LAYERC_TEST_CASE(control_protocol_contracts),
    LAYERC_TEST_CASE(control_dispatch_contracts),
    LAYERC_TEST_CASE(control_socket_contracts))
