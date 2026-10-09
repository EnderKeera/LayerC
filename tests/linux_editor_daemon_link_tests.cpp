#include "control_channel.hpp"
#include "daemon_host.hpp"
#include "daemon_link.hpp"
#include "test_support.hpp"

#include "layerc/core/frame.hpp"
#include "layerc/core/utf8.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace control = layerc::platform;
using layerc::editor::ReloadState;
using layerc::test::expect;

// A private runtime directory; the user's control socket is never reached.
class PrivateRuntime {
public:
  PrivateRuntime()
      : temp_("layerc-editor-link"),
        runtime_("XDG_RUNTIME_DIR", temp_.path() / "runtime") {
    std::filesystem::create_directories(temp_.path() / "runtime");
    (void)::chmod((temp_.path() / "runtime").c_str(), 0700);
  }

private:
  layerc::test::TempDirectory temp_;
  layerc::test::ScopedEnvironment runtime_;
};

// Serves LayerC.Control requests on the private socket until destroyed.
class FakeDaemon {
public:
  using Reply = std::function<std::optional<std::string>(std::string_view)>;

  explicit FakeDaemon(Reply reply) : reply_(std::move(reply)) {
    std::string error;
    std::string warning;
    listener_ = control::ControlListener::open(error, warning);
    expect(listener_.has_value(), "the fixture daemon listens: " + error);
    if (listener_)
      thread_ = std::jthread([this](const std::stop_token stop) { serve(stop); });
  }

private:
  void serve(const std::stop_token stop) {
    while (!stop.stop_requested()) {
      pollfd listening{listener_->fd(), POLLIN, 0};
      if (::poll(&listening, 1, 50) <= 0) continue;
      const auto accepted = listener_->accept();
      if (accepted.state != control::ControlAcceptResult::State::accepted) continue;
      layerc::core::FrameDecoder decoder{control::control_frame_limit,
                                         control::control_buffer_limit};
      std::optional<std::string> request;
      char chunk[4096];
      for (int waits = 0; !request && waits < 40; ++waits) {
        pollfd client{accepted.fd, POLLIN, 0};
        if (::poll(&client, 1, 50) <= 0) continue;
        const auto received = ::read(accepted.fd, chunk, sizeof(chunk));
        if (received <= 0) break;
        decoder.feed(layerc::core::text_bytes(
            std::string_view{chunk, static_cast<std::size_t>(received)}));
        request = decoder.next_frame();
      }
      std::string error;
      if (request) {
        if (const auto reply = reply_(*request))
          (void)control::control_send_frame(accepted.fd, *reply, error);
      }
      (void)::close(accepted.fd);
    }
  }

  Reply reply_;
  std::optional<control::ControlListener> listener_;
  std::jthread thread_;
};

std::string kernel_reply(const std::string_view request) {
  layerc::runtime::DaemonKernel kernel;
  (void)kernel.reconcile(kernel.demand());
  return layerc::daemon::handle_control_request(request, kernel).response;
}

void offline_contracts() {
  const PrivateRuntime runtime;
  const auto status = layerc::editor::query_daemon_status();
  expect(!status.running && !status.error.empty() &&
             status.summary().starts_with("LayerD is not running"),
         "no daemon is reported as not running with its reason");
  const auto reload = layerc::editor::request_daemon_reload();
  expect(reload.state == ReloadState::daemon_unavailable,
         "a reload without a daemon is an offline save, not a failure");
  const layerc::editor::CommitOutcome saved{};
  expect(layerc::editor::save_report_text(saved, reload) ==
             "Saved. LayerD is not running; the change applies when it starts.",
         "an offline save names the pending runtime application");
}

void running_contracts() {
  const PrivateRuntime runtime;
  const FakeDaemon daemon(kernel_reply);
  const auto status = layerc::editor::query_daemon_status();
  expect(status.running && status.error.empty() && !status.version.empty(),
         "a running daemon reports its version: " + status.error);
  expect(status.summary().starts_with("LayerD " + status.version),
         "the summary names the daemon version");
  const auto reload = layerc::editor::request_daemon_reload();
  expect(reload.state == ReloadState::acknowledged,
         "a running daemon acknowledges the reload: " + reload.error);
  expect(layerc::editor::save_report_text({}, reload) ==
             "Saved \u2014 LayerD acknowledged the reload request.",
         "persistence and reload acknowledgement are distinct from runtime application");
}

void refused_contracts() {
  const PrivateRuntime runtime;
  const FakeDaemon daemon([](std::string_view) {
    return control::encode_control_response(false, "fixture refused", {});
  });
  const auto reload = layerc::editor::request_daemon_reload();
  expect(reload.state == ReloadState::failed,
         "a refused reload after a save is a distinct failure");
  const auto text = layerc::editor::save_report_text({}, reload);
  expect(text.starts_with("Saved; LayerD's reload could not be confirmed"),
         "a failed reload never hides the successful save: " + text);
}

void lost_acknowledgement_is_uncertain() {
  const PrivateRuntime runtime;
  std::atomic<bool> requested{};
  const FakeDaemon daemon([&](const std::string_view request)
      -> std::optional<std::string> {
    layerc::runtime::DaemonKernel kernel;
    const auto reply = layerc::daemon::handle_control_request(request, kernel);
    requested.store(reply.reload_requested);
    return std::nullopt; // Request accepted; close without delivering its ACK.
  });
  const auto reload = layerc::editor::request_daemon_reload();
  const auto text = layerc::editor::save_report_text({}, reload);
  expect(requested.load() && reload.state == ReloadState::failed &&
             text.starts_with("Saved; LayerD's reload could not be confirmed"),
         "lost acknowledgement cannot prove that runtime reload did not occur");
}

void inspection_errors_are_not_offline() {
  const PrivateRuntime runtime;
  std::string warning;
  const auto path = control::control_socket_path(warning);
  expect(path.has_value(), "the private control path resolves");
  if (!path) return;
  std::filesystem::create_directories(std::filesystem::path(*path).parent_path());
  expect(::chmod(std::filesystem::path(*path).parent_path().c_str(), 0700) == 0,
         "the malformed socket fixture stays private");
  // ELOOP is deterministic even when tests run as root, unlike EACCES.
  std::filesystem::create_symlink(std::filesystem::path(*path).filename(), *path);
  const auto status = layerc::editor::query_daemon_status();
  expect(!status.running && !status.socket_missing && !status.error.empty() &&
             status.summary().starts_with("LayerD status unavailable"),
         "an inspection failure retains unavailable status and its cause");
  const auto reload = layerc::editor::request_daemon_reload();
  expect(reload.state == ReloadState::failed && !reload.error.empty() &&
             layerc::editor::save_report_text({}, reload)
                 .starts_with("Saved; LayerD's reload could not be confirmed"),
         "an uninspectable socket never masquerades as an offline save");
}

void invalid_runtime_path_is_not_offline() {
  const PrivateRuntime runtime;
  std::string warning;
  const auto path = control::control_socket_path(warning);
  expect(path.has_value(), "the malformed runtime fixture path resolves");
  if (!path) return;
  expect(layerc::test::write_file(std::filesystem::path(*path).parent_path(),
                                  "not a directory"),
         "create a private non-directory path component");
  const auto status = layerc::editor::query_daemon_status();
  const auto reload = layerc::editor::request_daemon_reload();
  expect(!status.socket_missing && !status.error.empty() &&
             status.summary().starts_with("LayerD status unavailable") &&
             reload.state == ReloadState::failed,
         "ENOTDIR must remain a path fault, not become an offline save");
}

void full_queue_is_bounded_and_not_unlinked() {
  using Clock = std::chrono::steady_clock;
  const PrivateRuntime runtime;
  std::string error, warning;
  auto listener = control::ControlListener::open(error, warning);
  expect(listener.has_value(), "the backlog fixture listens: " + error);
  if (!listener) return;
  struct Clients {
    std::vector<int> fds;
    ~Clients() { for (const auto fd : fds) (void)::close(fd); }
  } clients;
  const auto socket_path = control::control_socket_path(warning);
  ::sockaddr_un address{};
  expect(socket_path && socket_path->size() < sizeof(address.sun_path),
         "the backlog fixture has a bounded private socket path");
  if (!socket_path || socket_path->size() >= sizeof(address.sun_path)) return;
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path->data(), socket_path->size());
  bool full = false;
  for (int count = 0; count < 256; ++count) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    expect(fd >= 0, "create an owned backlog fixture client");
    if (fd < 0) return;
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&address),
                  sizeof(address)) != 0) {
      full = errno == EAGAIN || errno == EWOULDBLOCK;
      (void)::close(fd);
      break;
    }
    clients.fds.push_back(fd);
  }
  expect(full, "the fixture fills its private listen queue");
  if (!full) return;
  auto begin = Clock::now();
  const auto blocked = control::control_connect(
      error, begin + std::chrono::milliseconds(50));
  expect(!blocked && error == "control connection timed out" &&
             Clock::now() - begin < std::chrono::seconds(1),
         "a full listen queue cannot indefinitely block the client");
  if (blocked) (void)::close(*blocked);
  const auto path = control::control_socket_path(warning);
  struct stat before{}, after{};
  expect(path && ::lstat(path->c_str(), &before) == 0,
         "the live socket inode is recorded");
  begin = Clock::now();
  const auto second = control::ControlListener::open(error, warning);
  expect(!second && Clock::now() - begin < std::chrono::seconds(2) &&
             error.find("leaving it intact") != std::string::npos,
         "a bounded stale probe refuses an uncertain live socket");
  expect(path && ::lstat(path->c_str(), &after) == 0 &&
             before.st_dev == after.st_dev && before.st_ino == after.st_ino,
         "a full queue never causes live-socket unlink/replacement");
}

void commit_failure_text_contracts() {
  using layerc::editor::CommitResult;
  const auto failed = layerc::editor::save_report_text(
      {CommitResult::write_failed, layerc::platform::ProfileWriteError::lock_busy},
      std::nullopt);
  expect(failed == "Not saved. Profile write failed: profile store is busy.",
         "a write failure names the store reason: " + failed);
  expect(layerc::editor::save_report_text({CommitResult::blocked_by_sidecar, std::nullopt},
                                          layerc::editor::ReloadResult{})
             .starts_with("Not saved"),
         "a blocked commit never reports a reload");
}

} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(offline_contracts),
                 LAYERC_TEST_CASE(running_contracts),
                 LAYERC_TEST_CASE(refused_contracts),
                 LAYERC_TEST_CASE(lost_acknowledgement_is_uncertain),
                 LAYERC_TEST_CASE(inspection_errors_are_not_offline),
                 LAYERC_TEST_CASE(invalid_runtime_path_is_not_offline),
                 LAYERC_TEST_CASE(full_queue_is_bounded_and_not_unlinked),
                 LAYERC_TEST_CASE(commit_failure_text_contracts))
