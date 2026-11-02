#include "pipewire_capture.hpp"
#include "test_support.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

namespace {
using namespace layerc::platform;
using layerc::test::expect;

std::size_t process_entries(const char *path) {
  std::error_code error;
  std::size_t count{};
  for (const auto &entry : std::filesystem::directory_iterator(path, error)) {
    (void)entry;
    ++count;
  }
  expect(!error, "process resource fixture can be measured");
  return count;
}

std::map<std::string, std::string> fd_snapshot() {
  std::map<std::string, std::string> result;
  for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
    std::error_code error;
    const auto target = std::filesystem::read_symlink(entry.path(), error);
    if (!error) result.emplace(entry.path().filename().string(), target.string());
  }
  return result;
}

void pipewire_private_remote_lifecycle_has_no_threads() {
  EventLoop loop;
  const auto threads = process_entries("/proc/self/task");
  int sockets[2]{-1, -1};
  if (!expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
              "private fake remote socketpair opens")) return;
  const auto descriptors = process_entries("/proc/self/fd");
  const auto before = fd_snapshot();
  PipeWireCapture capture{loop, layerc::core::WindowId{7}, layerc::core::CaptureQuality::low};
  expect(capture.resources() == layerc::runtime::RuntimeResourceStats{}, "zero native resources without demand");
  PortalRemote remote;
  remote.fd = sockets[0]; remote.node = 99;
  const bool started = capture.start(std::move(remote));
  if (!started) {
    expect(capture.reason() == "pipewire_version_requires_1_2", "older runtime reports explicit no-thread incompatibility");
    expect(!std::getenv("LAYERC_TEST_REQUIRE_PIPEWIRE_NATIVE"), "native-required fixture must not take the version fallback");
    expect(::fcntl(sockets[0], F_GETFD) < 0, "remote consumed even on startup failure");
    expect(capture.resources() == layerc::runtime::RuntimeResourceStats{}, "failed startup releases every native resource");
  } else {
    expect(capture.active() && capture.resources().threads == 0 && capture.resources().capture_sessions == 1,
           "one demand owns a stream and zero data-loop threads");
    // This fake server never approves or sends video. The core transport still
    // exercises fd integration without connecting to a production PipeWire
    // socket, desktop, microphone, or camera.
    for (int index = 0; index < 8; ++index) {
      (void)loop.poll_once(0);
      capture.dispatch(loop.ready_fds());
    }
    const auto result = capture.poll(layerc::core::MonotonicClock::now()).result;
    expect(!result.frame && result.backend == layerc::core::CaptureBackendKind::pipewire_portal &&
               result.diagnostic == "portal_pending", "no invented video before the remote supplies a frame");
    expect(!loop.poll_once(10).any(), "connected but idle transport blocks instead of ticking");
  }
  expect(process_entries("/proc/self/task") == threads, "library startup did not create a hidden helper thread");
  capture.stop();
  expect(!capture.active() && capture.resources() == layerc::runtime::RuntimeResourceStats{},
         "demand drop releases stream, context, core, loop and cache");
  const auto after = fd_snapshot();
  if (process_entries("/proc/self/fd") != descriptors - 1) {
    for (const auto &[fd, target] : before) std::cerr << "BEFORE fd " << fd << " = " << target << '\n';
    for (const auto &[fd, target] : after) std::cerr << "AFTER fd " << fd << " = " << target << '\n';
  }
  expect(process_entries("/proc/self/task") == threads &&
             process_entries("/proc/self/fd") == descriptors - 1,
         "quiescence returns actual process thread/fd counts to baseline");
  ::close(sockets[1]);
  expect(!loop.poll_once(10).any(), "quiesced transport has no registered fd or timer");
}

void pipewire_disconnect_and_config_override_are_visible() {
  EventLoop loop;
  const auto threads = process_entries("/proc/self/task");
  const auto descriptors = process_entries("/proc/self/fd");
  int sockets[2]{-1, -1};
  if (!expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
              "disconnect socketpair opens")) return;
  PipeWireCapture capture{loop, layerc::core::WindowId{8}, layerc::core::CaptureQuality::balanced};
  PortalRemote remote; remote.fd = sockets[0]; remote.node = 10;
  const bool started = capture.start(std::move(remote));
  ::close(sockets[1]);
  if (started) {
    const auto deadline = layerc::core::MonotonicClock::now() + std::chrono::seconds(2);
    while (capture.active() && layerc::core::MonotonicClock::now() < deadline) {
      (void)loop.poll_once(5);
      capture.dispatch(loop.ready_fds());
    }
    expect(!capture.active() && !capture.reason().empty(), "remote disconnection is a sticky, resource-free fault");
    const auto reason = capture.reason();
    for (int index = 0; index < 10; ++index) capture.dispatch({});
    expect(capture.reason() == reason && capture.resources() == layerc::runtime::RuntimeResourceStats{},
           "refresh does not retry a broken remote");
  } else expect(capture.reason() == "pipewire_version_requires_1_2", "version fallback is explicit");

  capture.stop();
  const layerc::test::ScopedEnvironment override{"PIPEWIRE_CONFIG_NAME", std::filesystem::path{"user-exec.conf"}};
  expect(override.applied(), "config-override fixture applied");
  if (!expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
              "override socketpair opens")) return;
  PortalRemote overridden; overridden.fd = sockets[0]; overridden.node = 10;
  expect(!capture.start(std::move(overridden)) &&
             capture.reason() == (started ? "pipewire_config_override" : "pipewire_version_requires_1_2"),
         "custom config cannot silently execute helpers or enable data-loop threads");
  ::close(sockets[1]);
  expect(capture.resources() == layerc::runtime::RuntimeResourceStats{} &&
             process_entries("/proc/self/task") == threads &&
             process_entries("/proc/self/fd") == descriptors,
         "configuration refusal closes the transferred fd and starts no threads");
}
void pipewire_repeated_and_overlapping_demand_is_quiescent() {
  EventLoop loop;
  const auto threads = process_entries("/proc/self/task");
  const auto descriptors = process_entries("/proc/self/fd");
  PipeWireCapture first{loop, layerc::core::WindowId{11}, layerc::core::CaptureQuality::low};
  PipeWireCapture second{loop, layerc::core::WindowId{12}, layerc::core::CaptureQuality::balanced};
  for (int cycle = 0; cycle < 100; ++cycle) {
    int peers[2]{-1, -1};
    bool started[2]{};
    for (int index = 0; index < 2; ++index) {
      int sockets[2]{-1, -1};
      if (!expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
                  "cycle fixture socketpair opens")) return;
      peers[index] = sockets[1];
      PortalRemote remote;
      remote.fd = sockets[0]; remote.node = static_cast<std::uint32_t>(100 + index);
      auto &capture = index == 0 ? first : second;
      started[index] = capture.start(std::move(remote));
      expect(started[index] || capture.reason() == "pipewire_version_requires_1_2",
             "overlapping capture either starts or reports the old-runtime fault");
      expect(started[index] || !std::getenv("LAYERC_TEST_REQUIRE_PIPEWIRE_NATIVE"),
             "native-required cycles do not bypass native ownership");
    }
    expect(process_entries("/proc/self/task") == threads, "overlapping demand creates no threads");
    for (int dispatch = 0; dispatch < 4; ++dispatch) {
      (void)loop.poll_once(0);
      first.dispatch(loop.ready_fds());
      second.dispatch(loop.ready_fds());
    }
    first.stop();
    if (started[1]) expect(second.active(), "dropping one library lease preserves the other stream");
    ::close(peers[0]);
    (void)loop.poll_once(0);
    second.dispatch(loop.ready_fds());
    second.stop();
    ::close(peers[1]);
    expect(first.resources() == layerc::runtime::RuntimeResourceStats{} &&
               second.resources() == layerc::runtime::RuntimeResourceStats{} &&
               process_entries("/proc/self/task") == threads &&
               process_entries("/proc/self/fd") == descriptors,
           "each cycle returns both streams and actual fds/threads to baseline");
  }
  expect(!loop.poll_once(10).any(), "100 cycles leave the loop idle-blocking");
}

// F05: a portal grant selects a stream; it never verifies that the stream is
// the session's target window, so the binding says which choice produced it.
void pipewire_portal_streams_report_their_binding() {
  EventLoop loop;
  const auto now = layerc::core::MonotonicClock::now();
  {
    PipeWireCapture window_stream{loop, layerc::core::WindowId{9},
                                  layerc::core::CaptureQuality::low};
    // No remote is started: the binding still describes the requested source,
    // and every result carries it.
    const auto idle = window_stream.poll(now).result;
    expect(idle.binding ==
               layerc::capture::CaptureTargetBinding::user_selected_window,
           "a window stream is a user-selected source, never a target binding");
  }
  {
    PipeWireCapture monitor_stream{loop, layerc::core::WindowId{9},
                                   layerc::core::CaptureQuality::low};
    int sockets[2]{-1, -1};
    if (!expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
                "monitor-stream socketpair opens"))
      return;
    PortalRemote remote;
    remote.fd = sockets[0];
    remote.node = 12;
    remote.monitor = true;
    remote.geometry = LogicalRect{0, 0, 1920, 1080};
    (void)monitor_stream.start(std::move(remote));
    expect(monitor_stream.poll(now).result.binding ==
               layerc::capture::CaptureTargetBinding::monitor_crop,
           "a monitor source is the explicit degraded monitor crop");
    monitor_stream.stop();
    expect(monitor_stream.poll(now).result.binding ==
               layerc::capture::CaptureTargetBinding::user_selected_window,
           "the binding follows the source the portal selected");
    ::close(sockets[1]);
  }
}
} // namespace

LAYERC_TEST_MAIN(LAYERC_TEST_CASE(pipewire_private_remote_lifecycle_has_no_threads),
                 LAYERC_TEST_CASE(pipewire_disconnect_and_config_override_are_visible),
                 LAYERC_TEST_CASE(pipewire_repeated_and_overlapping_demand_is_quiescent),
                 LAYERC_TEST_CASE(pipewire_portal_streams_report_their_binding))
