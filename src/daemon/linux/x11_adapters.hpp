#pragma once

#include "daemon_host.hpp"
#include "display_dispatch.hpp"
#include "event_loop.hpp"

#include "layerc/platform/profile_store.hpp"
#include "layerc/runtime/daemon_kernel.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace layerc::daemon {

using X11Dispatch = DisplayDispatch;

// The daemon's single compile-time X11 seam. The no-X11 build keeps the same
// orchestration surface and reports unavailable capabilities without linking
// any Xlib adapter.
class X11Adapters {
public:
  X11Adapters();
  ~X11Adapters();
  X11Adapters(const X11Adapters &) = delete;
  X11Adapters &operator=(const X11Adapters &) = delete;

  [[nodiscard]] layerc::runtime::RuntimeServiceRegistry
  runtime_services() noexcept;

  void start(layerc::platform::EventLoop &loop, LinuxStatusSnapshot &status,
             std::vector<std::string> &diagnostics);
  void refresh(std::span<const layerc::platform::StoredProfile> profiles,
               bool configuration_changed,
               layerc::runtime::DaemonKernel &kernel,
               LinuxStatusSnapshot &status,
               std::vector<std::string> &diagnostics);
  void refresh_status(const layerc::runtime::DaemonKernel &kernel,
                      LinuxStatusSnapshot &status) const;
  [[nodiscard]] X11Dispatch dispatch(std::span<const int> ready);
  // Queue-aware prepare/check phase for the blocking loop: Xlib synchronous
  // queries buffer events client-side without waking the socket, so the host
  // must check pending_events() before sleeping and dispatch buffered work
  // without waiting for a second external event. The loop still blocks when
  // both local queues and external sources are empty (no polling timer).
  [[nodiscard]] bool has_pending_events() noexcept;
  [[nodiscard]] X11Dispatch dispatch_pending();
  // True when a completed retired checkpoint changes automation demand.
  [[nodiscard]] bool render(layerc::core::TimePoint now);
  void stop_sessions(layerc::core::TimePoint now);
  [[nodiscard]] std::optional<layerc::core::TimePoint>
  next_deadline(layerc::core::TimePoint now) const;
  [[nodiscard]] std::size_t session_count() const noexcept;
  [[nodiscard]] std::vector<layerc::runtime::SessionDiagnostics>
  session_diagnostics() const;
  [[nodiscard]] TraceControlResult trace_request(const layerc::core::TraceControlRequest &request);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace layerc::daemon
