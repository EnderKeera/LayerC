#pragma once

#include "automation_trace_client.hpp"
#include "layerc/core/automation.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace layerc::editor {

// The Windows editor's automation trace subscription. LayerD pushes trace
// packets over the live pipe; the relay buffers their replay roots and hands
// them out as the polled replies AutomationTraceClient returns, like the
// Linux control channel. One subscription at a time: a newer one replaces
// the older, whose updates end. The buffer keeps the newest roots and
// reports dropped ones as a gap. Not synchronized: DaemonHub guards it.
class TraceRelay final {
public:
  // Subscribe attempts that may fail in a row before the subscription ends.
  static constexpr int max_consecutive_failures = 3;
  static constexpr std::size_t max_buffered_events =
      layerc::core::max_automation_trace_entries;

  // Starts a fresh subscription with its first attempt pending and returns
  // its nonzero id.
  [[nodiscard]] std::uint64_t subscribe();
  // Ends the subscription when id is current. Returns whether LayerD may
  // still hold a subscription on this connection, so it should be told.
  [[nodiscard]] bool unsubscribe(std::uint64_t id) noexcept;
  [[nodiscard]] bool current(std::uint64_t id) const noexcept {
    return id != 0 && id == subscriber_;
  }

  // Whether id needs another subscribe attempt; if so, marks it pending.
  [[nodiscard]] bool begin_attempt(std::uint64_t id) noexcept;
  void attempt_succeeded(std::uint64_t id) noexcept;
  void attempt_failed(std::uint64_t id, std::string reason);
  // Only a matching acknowledgement or a disconnect clears the wire state.
  void unsubscribe_succeeded() noexcept;
  // The live pipe closed. A held subscription is resubscribed on the next
  // poll and reports a gap instead of clearing the recorded roots.
  void connection_lost() noexcept;
  // Ends the current subscription; its next update carries the reason.
  void fail(std::string reason);
  // One automation_trace_event packet. Ignored unless subscribed.
  void append_packet(std::string_view packet);

  [[nodiscard]] std::optional<AutomationTraceUpdate>
  take_update(std::uint64_t id);

private:
  void clear_subscription() noexcept;

  std::uint64_t subscriber_{};
  std::uint64_t next_id_{};
  int failures_{};
  bool attempting_{};
  bool subscribed_{};
  // LayerD holds a subscription on the current connection, for any id.
  bool held_{};
  bool lapsed_{};
  bool reset_pending_{};
  bool gap_pending_{};
  bool finished_{};
  std::optional<std::string> error_;
  std::deque<layerc::core::AutomationEvent> events_;
  std::uint64_t delivered_{};
};

} // namespace layerc::editor
