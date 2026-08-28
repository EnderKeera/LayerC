#include "layerc/platform/live_channel.hpp"
#include "platform_live_scenarios.hpp"
#include "test_support.hpp"

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

#include <windows.h>

namespace {

using layerc::test::expect;

int run_authenticated_peer(const bool server, const std::wstring &pipe_name) {
  using Channel = layerc::platform::NamedPipeEditorDaemonChannel;
  Channel channel(server ? Channel::Role::server : Channel::Role::client,
                  pipe_name);
  bool sent{};
  for (int attempt = 0; attempt < 5'000; ++attempt) {
    for (auto &result : channel.poll()) {
      const auto *message = std::get_if<layerc::core::LiveMessage>(&result);
      if (message == nullptr)
        continue;
      if (server && message->type == layerc::core::LiveMessageType::handshake) {
        (void)channel.send({
            .type = layerc::core::LiveMessageType::acknowledgement,
            .revision = message->revision,
            .text = "authenticated",
        });
        sent = true;
      } else if (!server &&
                 message->type ==
                     layerc::core::LiveMessageType::acknowledgement &&
                 message->text == "authenticated") {
        return 0;
      }
    }
    if (!server && channel.connected() && !sent) {
      sent = channel.send({.type = layerc::core::LiveMessageType::handshake,
                           .revision = 1,
                           .text = "sibling-auth-test"});
    }
    if (server && sent) {
      // Give the overlapped acknowledgement write time to complete.
      for (int flush = 0; flush < 20; ++flush) {
        (void)channel.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return 1;
}


} // namespace

int main(const int argc, char *argv[]) {
  if (argc == 3 && (std::string_view(argv[1]) == "--auth-server" ||
                    std::string_view(argv[1]) == "--auth-client")) {
    const std::string pipe_utf8(argv[2]);
    return run_authenticated_peer(
        std::string_view(argv[1]) == "--auth-server",
        std::wstring(pipe_utf8.begin(), pipe_utf8.end()));
  }
  bool passed = true;
  passed &= expect(
      std::wstring_view(layerc::platform::live_edit_pipe_name) ==
          L"\\\\.\\pipe\\LayerC.LayerD.LiveEdit.v3",
      "the tested production pipe name should match the protocol contract");
  passed &= run_profile_platform_scenarios();
  const std::wstring spoof_pipe_name =
      L"\\\\.\\pipe\\LayerC.PlatformSpoofTest." +
      std::to_wstring(GetCurrentProcessId()) + L"." +
      std::to_wstring(GetTickCount64());
  {
    layerc::platform::NamedPipeEditorDaemonChannel spoof_server(
        layerc::platform::NamedPipeEditorDaemonChannel::Role::server,
        spoof_pipe_name);
    layerc::platform::NamedPipeEditorDaemonChannel guarded_client(
        layerc::platform::NamedPipeEditorDaemonChannel::Role::client,
        spoof_pipe_name, GetCurrentProcessId() + 1U);
    for (int attempt = 0; attempt < 50; ++attempt) {
      (void)guarded_client.poll();
      (void)spoof_server.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    passed &= expect(!guarded_client.connected() &&
                         !guarded_client.last_error().empty(),
                     "the client should reject an unexpected pipe server PID");
  }

  const std::wstring pipe_name = L"\\\\.\\pipe\\LayerC.PlatformTest." +
                                 std::to_wstring(GetCurrentProcessId()) + L"." +
                                 std::to_wstring(GetTickCount64());
  layerc::platform::NamedPipeEditorDaemonChannel server(
      layerc::platform::NamedPipeEditorDaemonChannel::Role::server, pipe_name,
      GetCurrentProcessId());
  layerc::platform::NamedPipeEditorDaemonChannel client(
      layerc::platform::NamedPipeEditorDaemonChannel::Role::client, pipe_name,
      GetCurrentProcessId());
  std::array<HANDLE, 3> wait_handles{};
  passed &=
      expect(server.wait_handles(wait_handles) == 1,
             "a listening server should expose one waitable connect event");
  for (int attempt = 0;
       attempt < 1000 && (!server.connected() || !client.connected());
       ++attempt) {
    (void)client.poll();
    (void)server.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  passed &= expect(server.connected() && client.connected(),
                   "same-user overlapped pipe peers should connect");
  passed &= expect(server.wait_handles(wait_handles) >= 1,
                   "a connected server should expose waitable overlapped I/O");
  passed &=
      expect(server.peer_process_id() == GetCurrentProcessId() &&
                 client.peer_process_id() == GetCurrentProcessId(),
             "both authenticated channel ends should retain the peer PID");
  passed &= expect(client.send({
                       .type = layerc::core::LiveMessageType::handshake,
                       .revision = 77,
                       .text = "platform-test",
                   }),
                   "client should queue a live message");

  bool server_received{};
  for (int attempt = 0; attempt < 1000 && !server_received; ++attempt) {
    (void)client.poll();
    for (auto &result : server.poll()) {
      const auto *message = std::get_if<layerc::core::LiveMessage>(&result);
      server_received = message != nullptr && message->revision == 77 &&
                        message->text == "platform-test";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  passed &= expect(server_received,
                   "framed UTF-8 messages should cross the named pipe");

  return passed ? 0 : 1;
}
