#pragma once

#include "layerc/core/live_draft_control.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace layerc::editor {
struct LiveDraftUpdate {
  std::optional<layerc::core::LiveDraftReply> reply;
  std::string error;
  // The publish() call this result belongs to (renewals keep their lease's).
  std::uint64_t sequence{};
};
// One idle-blocking worker per editor with bounded authenticated local I/O.
// Publishes coalesce and end requests queue without blocking the caller; the
// worker serializes them, so a profile switch ends the previous lease before
// the next profile's publish. Only owned values cross its mutex. Destruction
// joins and ends any lease (expiry handles an unavailable daemon). notify_fd()
// becomes readable only for publish replies and failures, never for silent
// renewals or ends, so the UI thread can block on it instead of polling.
class LiveDraftClient final {
public:
  LiveDraftClient();
  ~LiveDraftClient();
  LiveDraftClient(const LiveDraftClient &) = delete;
  LiveDraftClient &operator=(const LiveDraftClient &) = delete;
  // Returns the sequence that results for this publish will carry.
  std::uint64_t publish(std::string profile_id, std::string profile_json);
  // Queues the end of the current lease and discards an unsent publish.
  void end();
  // Drains notify_fd() and returns the latest result, if any.
  [[nodiscard]] std::optional<LiveDraftUpdate> take_update();
  [[nodiscard]] int notify_fd() const noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace layerc::editor
