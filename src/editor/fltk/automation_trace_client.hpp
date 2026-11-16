#pragma once
#include "layerc/core/automation_trace_control.hpp"
#include <memory>

namespace layerc::editor {
struct AutomationTraceUpdate {
  std::optional<layerc::core::TraceControlReply> reply;
  std::string error;
};
// Demand-created, idle-blocking, bounded local I/O. Updates cross a mutex as
// owned value copies; the worker never calls UI objects.
class AutomationTraceClient final {
public:
  explicit AutomationTraceClient(std::string profile_id);
  ~AutomationTraceClient();
  AutomationTraceClient(const AutomationTraceClient &) = delete;
  AutomationTraceClient &operator=(const AutomationTraceClient &) = delete;
  void request_poll();
  [[nodiscard]] std::optional<AutomationTraceUpdate> take_update();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace layerc::editor
