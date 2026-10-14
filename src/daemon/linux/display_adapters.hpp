#pragma once

#include "daemon_host.hpp"
#include "display_dispatch.hpp"
#include "display_selection.hpp"

#include <memory>

namespace layerc::daemon {

// The daemon's one display-server-neutral orchestration seam. Connections and
// providers are owned here; the host never branches on the selected backend.
class DisplayAdapters {
public:
  DisplayAdapters();
  ~DisplayAdapters();
  DisplayAdapters(const DisplayAdapters &) = delete;
  DisplayAdapters &operator=(const DisplayAdapters &) = delete;

  [[nodiscard]] layerc::runtime::RuntimeServiceRegistry runtime_services() noexcept;
  // The user's input opt-in; call before start().
  void configure_input(const InputOptions &options);
  void start(layerc::platform::EventLoop &loop, LinuxStatusSnapshot &status,
             std::vector<std::string> &diagnostics);
  void refresh(std::span<const layerc::platform::StoredProfile> profiles,
               bool configuration_changed, layerc::runtime::DaemonKernel &kernel,
               LinuxStatusSnapshot &status, std::vector<std::string> &diagnostics);
  void refresh_status(const layerc::runtime::DaemonKernel &kernel,
                      LinuxStatusSnapshot &status) const;
  [[nodiscard]] DisplayDispatch dispatch(std::span<const int> ready);
  [[nodiscard]] bool has_pending_events();
  [[nodiscard]] DisplayDispatch dispatch_pending();
  [[nodiscard]] bool render(layerc::core::TimePoint now);
  void stop_sessions(layerc::core::TimePoint now);
  [[nodiscard]] std::optional<layerc::core::TimePoint>
  next_deadline(layerc::core::TimePoint now) const;
  [[nodiscard]] std::size_t session_count() const noexcept;
  [[nodiscard]] std::vector<layerc::runtime::SessionDiagnostics> session_diagnostics() const;
  [[nodiscard]] TraceControlResult trace_request(const layerc::core::TraceControlRequest &request);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace layerc::daemon
