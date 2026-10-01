#include "control_channel.hpp"
#include "daemon_host.hpp"
#include "event_loop.hpp"
#include "layerc/platform/profile_store.hpp"
#include "test_support.hpp"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace {
namespace control = layerc::platform;
using layerc::test::expect;
using Clock = std::chrono::steady_clock;

// An owned, joined test responder; no production daemon or store is contacted.
class ResponseFixture {
public:
  explicit ResponseFixture(std::string response) {
    std::string error;
    std::string warning;
    listener_ = control::ControlListener::open(error, warning);
    if (!expect(listener_.has_value(), "response fixture listener opens")) return;
    worker_ = std::jthread([this, response = std::move(response)] {
      const auto deadline = Clock::now() + std::chrono::seconds(3);
      while (Clock::now() < deadline) {
        pollfd watch{listener_->fd(), POLLIN, 0};
        if (::poll(&watch, 1, 50) <= 0) continue;
        const auto peer = listener_->accept();
        if (peer.state != control::ControlAcceptResult::State::accepted) continue;
        control::ControlFramer framer;
        std::array<char, 4096> bytes{};
        while (Clock::now() < deadline) {
          pollfd request{peer.fd, POLLIN, 0};
          if (::poll(&request, 1, 50) <= 0) continue;
          const auto count = ::read(peer.fd, bytes.data(), bytes.size());
          if (count <= 0) break;
          framer.feed({bytes.data(), static_cast<std::size_t>(count)});
          if (framer.next_frame().has_value()) {
            answered_ = control::control_send_frame(peer.fd, response, error_);
            break;
          }
        }
        (void)::close(peer.fd);
        return;
      }
    });
  }
  void finish() {
    if (worker_.joinable()) worker_.join();
    (void)expect(answered_, "fixture delivered its framed response");
  }
private:
  std::optional<control::ControlListener> listener_;
  std::string error_;
  bool answered_{};
  std::jthread worker_; // joins before the listener and result fields die
};

struct CommandResult { int exit_code{-1}; std::string output; };

CommandResult run_status(bool json) {
  int output[2]{};
  CommandResult result;
  if (!expect(::pipe(output) == 0, "CLI output pipe opens")) return result;
  posix_spawn_file_actions_t actions{};
  if (posix_spawn_file_actions_init(&actions) != 0) {
    (void)::close(output[0]);
    (void)::close(output[1]);
    return result;
  }
  (void)posix_spawn_file_actions_addclose(&actions, output[0]);
  (void)posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
  (void)posix_spawn_file_actions_addclose(&actions, output[1]);
  (void)posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  std::string command = "status";
  std::string option = "--json";
  std::string executable = LAYERC_TEST_LAYERCTL_PATH;
  std::array<char *, 4> args{executable.data(), command.data(), json ? option.data() : nullptr, nullptr};
  pid_t child{};
  const int spawned = ::posix_spawn(&child, executable.c_str(), &actions, nullptr, args.data(), environ);
  (void)posix_spawn_file_actions_destroy(&actions);
  (void)::close(output[1]);
  if (spawned == 0) {
    std::array<char, 4096> bytes{};
    for (;;) {
      const auto count = ::read(output[0], bytes.data(), bytes.size());
      if (count > 0) result.output.append(bytes.data(), static_cast<std::size_t>(count));
      else if (count < 0 && errno == EINTR) continue;
      else break;
    }
    int status{};
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
  }
  (void)::close(output[0]);
  return result;
}
} // namespace

void control_response_acceptance_contracts() {
  const layerc::test::TempDirectory root("layerc-response");
  const layerc::test::ScopedEnvironment runtime("XDG_RUNTIME_DIR", root.path());
  if (!expect(runtime.applied(), "response test runtime is private")) return;
  const std::string success = control::encode_control_response(true, {}, R"({"pid":42})");
  const std::vector<std::string> rejected{
      "not json", R"({"ok":true})", R"({"proto":"LayerE","v":1,"ok":true})",
      R"({"proto":"LayerC.Control","v":2,"ok":true})",
      R"({"proto":"LayerC.Control","v":1,"ok":"true"})",
      control::encode_control_response(false, "reload refused", {})};
  for (const auto &response : rejected) {
    for (const bool json : {false, true}) {
      ResponseFixture fixture(response);
      const auto result = run_status(json);
      fixture.finish();
      (void)expect(result.exit_code == 1 && result.output.empty(),
                   "human and JSON CLI reject invalid or failed responses without success output");
    }
    ResponseFixture fixture(response);
    (void)expect(!control::signal_profile_reload(), "reload notification rejects framed but unaccepted responses");
    fixture.finish();
  }
  for (const bool json : {false, true}) {
    ResponseFixture fixture(success);
    const auto result = run_status(json);
    fixture.finish();
    (void)expect(result.exit_code == 0 && (!json || result.output == success + "\n"),
                 "accepted JSON retains original wire bytes; human mode succeeds");
  }
  ResponseFixture fixture(success);
  (void)expect(control::signal_profile_reload(), "valid reload acknowledgement succeeds");
  fixture.finish();
}

void control_response_backpressure_contracts() {
  const layerc::test::TempDirectory root("layerc-backpressure");
  const layerc::test::ScopedEnvironment runtime("XDG_RUNTIME_DIR", root.path());
  control::EventLoop loop;
  layerc::daemon::ControlService service;
  std::string error;
  std::string warning;
  if (!expect(runtime.applied() && loop.valid() && service.start(error, warning), "backpressure service starts")) return;
  service.attach(loop);
  layerc::runtime::DaemonKernel kernel;
  layerc::daemon::LinuxStatusSnapshot large;
  // Advertised extensions avoid repeating the connection reason in each
  // unavailable-extension row and accidentally exceeding the frame ceiling.
  large.x11 = {true, {true, 2, 4}, {true, 1, 1}, {true, 1, 6},
               {true, 0, 4}, {true, 1, 1}, {true, 6, 0}, true,
               std::string(200000, 'x')};
  const auto response_bytes = layerc::daemon::status_payload_json(kernel, large).size();
  if (!expect(response_bytes > 64U * 1024U && response_bytes < control::control_frame_limit,
              "slow-reader fixture fits the frame ceiling and exceeds one write budget")) {
    service.shutdown(loop);
    return;
  }

  const auto start_slow = [&]() -> std::optional<int> {
    const auto peer = control::control_connect(error);
    if (!peer.has_value()) return std::nullopt;
    (void)service.service(loop, {service.listener_fd()}, kernel);
    if (!control::control_send_frame(*peer, control::encode_control_request(control::ControlCommand::status), error)) {
      (void)::close(*peer);
      return std::nullopt;
    }
    (void)loop.poll_once(1000);
    for (const int fd : loop.ready_fds()) {
      if (fd != service.listener_fd()) {
        const int capacity = 1024;
        (void)expect(::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) == 0,
                     "response socket capacity is deliberately constrained");
      }
    }
    const auto begin = Clock::now();
    (void)service.service(loop, loop.ready_fds(), kernel, large);
    (void)expect(Clock::now() - begin < std::chrono::milliseconds(500) && service.client_count() == 1,
                 "queued response never privately waits on a slow reader");
    return peer;
  };
  const auto slow = start_slow();
  if (!expect(slow.has_value(), "slow reader connects")) { service.shutdown(loop); return; }
  const auto quick = control::control_connect(error);
  if (expect(quick.has_value(), "second client connects despite backpressure")) {
    (void)service.service(loop, {service.listener_fd()}, kernel);
    (void)expect(control::control_send_frame(*quick, control::encode_control_request(control::ControlCommand::reload), error), "second request writes");
    const auto deadline = Clock::now() + std::chrono::milliseconds(20);
    loop.set_deadline(deadline);
    bool timer_fired = false;
    control::ControlFramer response;
    std::optional<std::string> reply;
    const auto limit = Clock::now() + std::chrono::milliseconds(500);
    while (Clock::now() < limit && (!timer_fired || !reply.has_value())) {
      const auto wake = loop.poll_once(100);
      timer_fired = timer_fired || wake.timer;
      (void)service.service(loop, loop.ready_fds(), kernel);
      std::array<char, 4096> bytes{};
      const auto count = ::read(*quick, bytes.data(), bytes.size());
      if (count > 0) response.feed({bytes.data(), static_cast<std::size_t>(count)});
      if (!reply.has_value()) reply = response.next_frame();
    }
    const auto decoded = reply.has_value() ? control::decode_control_response(*reply)
                                           : std::optional<control::ControlResponse>{};
    (void)expect(timer_fired && decoded.has_value() && decoded->ok,
                 "daemon deadline and another control client remain serviceable");
    (void)::close(*quick);
  }
  // Resume reading: partial writes must retain the exact frame offset.
  control::ControlFramer response;
  std::optional<std::string> reply;
  const auto limit = Clock::now() + std::chrono::seconds(2);
  while (Clock::now() < limit && !reply.has_value()) {
    std::array<char, 8192> bytes{};
    const auto count = ::read(*slow, bytes.data(), bytes.size());
    if (count > 0) response.feed({bytes.data(), static_cast<std::size_t>(count)});
    reply = response.next_frame();
    (void)loop.poll_once(10);
    (void)service.service(loop, loop.ready_fds(), kernel, large);
  }
  const auto decoded = reply.has_value() ? control::decode_control_response(*reply)
                                         : std::optional<control::ControlResponse>{};
  (void)expect(decoded.has_value() && decoded->ok &&
                   reply->find("\"reason\":\"" + large.x11.reason + "\"") != std::string::npos &&
                   service.client_count() == 0,
               "resumed reader receives the complete uncorrupted frame and releases client state");
  (void)::close(*slow);
  const auto expiring = start_slow();
  if (expect(expiring.has_value(), "expiring response fixture connects")) {
    service.expire_clients(loop, Clock::now() + layerc::daemon::control_client_timeout + layerc::core::Milliseconds{1});
    (void)expect(service.client_count() == 0 && !service.next_deadline().has_value(),
                 "queued response expires without waiting for peer reads");
    (void)::close(*expiring);
  }
  service.shutdown(loop);
}

void control_server_identity_contracts() {
  if (::geteuid() != 0) {
    std::puts("foreign-server credential variation unavailable: test UID is not root");
    return;
  }
  const layerc::test::TempDirectory root("layerc-foreign-server");
  const auto directory = root.path() / "layerc";
  std::filesystem::create_directories(directory);
  (void)::chmod(root.path().c_str(), 0755);
  (void)::chmod(directory.c_str(), 0777); // Deliberately unsafe disposable pathname.
  const layerc::test::ScopedEnvironment runtime("XDG_RUNTIME_DIR", root.path());
  if (!expect(runtime.applied(), "foreign server runtime is isolated")) return;
  int ready[2]{};
  if (!expect(::pipe(ready) == 0, "foreign server readiness pipe opens")) return;
  const auto child = ::fork();
  if (child == 0) {
    (void)::close(ready[0]);
    if (::setgid(65534) != 0 || ::setuid(65534) != 0) _exit(2);
    const int server = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = (directory / "control.sock").string();
    if (path.size() >= sizeof(address.sun_path)) _exit(3);
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (server < 0 || ::bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || ::listen(server, 1) != 0) _exit(4);
    if (::write(ready[1], "x", 1) != 1) _exit(5);
    (void)::close(ready[1]);
    pollfd watch{server, POLLIN, 0};
    if (::poll(&watch, 1, 3000) <= 0) _exit(6);
    const int peer = ::accept(server, nullptr, nullptr);
    if (peer < 0) _exit(7);
    pollfd incoming{peer, POLLIN, 0};
    if (::poll(&incoming, 1, 3000) <= 0) _exit(8);
    char byte{};
    const auto count = ::read(peer, &byte, 1);
    (void)::close(peer);
    (void)::close(server);
    _exit(count == 0 ? 0 : 9); // Authentication must precede all request bytes.
  }
  (void)::close(ready[1]);
  if (expect(child > 0, "foreign server child starts")) {
    char byte{};
    if (expect(::read(ready[0], &byte, 1) == 1, "foreign server is ready")) {
      std::string error;
      const auto peer = control::control_connect(error);
      (void)expect(!peer.has_value() && error.find("different effective uid") != std::string::npos,
                   "client refuses a reachable foreign-UID server before sending a request");
      if (peer.has_value()) (void)::close(*peer);
    }
    int status{};
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    (void)expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "foreign listener observed authenticated-client close without request bytes");
  }
  (void)::close(ready[0]);
}
