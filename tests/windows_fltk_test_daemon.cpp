#include "editor_test_environment.hpp"
#include "platform/win32/daemon_hub.hpp"

namespace {
std::vector<std::unique_ptr<layerc::editor::DaemonHub>> hubs;
}

namespace layerc::test {

void push_editor_test_daemon(const std::filesystem::path &identity) {
  hubs.push_back(std::make_unique<editor::DaemonHub>(editor::DaemonHubEnvironment{
      .pipe_name = L"\\\\.\\pipe\\" + identity.native(),
      .expected_server_process_id = GetCurrentProcessId(),
      .start_daemon = {},
      .signal_reload = {},
  }));
}

void pop_editor_test_daemon() noexcept { hubs.pop_back(); }

} // namespace layerc::test

namespace layerc::editor {

DaemonHub &product_daemon_hub() {
  if (hubs.empty()) throw std::logic_error("The Windows editor needs a private daemon fixture");
  return *hubs.back();
}

} // namespace layerc::editor
