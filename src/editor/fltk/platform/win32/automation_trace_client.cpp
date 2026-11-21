#include "automation_trace_client.hpp"

#include "platform/win32/daemon_hub.hpp"

#include <cstdint>

namespace layerc::editor {

// The Windows editor's trace rides the hub's authenticated LayerD pipe
// instead of a control socket. LayerD holds one subscription per pipe and
// pushes the trace of every session it runs, which the Win32 editor shows
// as well, so the profile id selects nothing here. The hub's worker does
// the I/O; these calls only touch its relay under the hub's lock.
struct AutomationTraceClient::Impl {
  explicit Impl(DaemonHub &owner)
      : hub(owner), subscription(owner.subscribe_trace()) {}

  DaemonHub &hub;
  std::uint64_t subscription;
};

AutomationTraceClient::AutomationTraceClient(std::string /*profile_id*/)
    : impl_(std::make_unique<Impl>(product_daemon_hub())) {}

AutomationTraceClient::~AutomationTraceClient() {
  impl_->hub.unsubscribe_trace(impl_->subscription);
}

void AutomationTraceClient::request_poll() {
  impl_->hub.poll_trace(impl_->subscription);
}

std::optional<AutomationTraceUpdate> AutomationTraceClient::take_update() {
  return impl_->hub.take_trace(impl_->subscription);
}

} // namespace layerc::editor
