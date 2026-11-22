#pragma once

#include "platform/on_window_edit.hpp"
#include "live_edit_session.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include <windows.h>

namespace layerc::editor {

// One UI call on the hub's on-window edit session, applied by the worker in
// order. Tagged with the session id the UI holds.
struct HubEditCommand {
  enum class Kind : std::uint8_t { begin, live_profile, anchor, direct_drag, end };
  Kind kind{Kind::end};
  std::uint64_t id{};
  std::uint64_t window{};
  OnWindowEditBegin payload{};
  std::string panel_id{};
  double x{};
  double y{};
  bool flag{}; // anchor: final; direct_drag: enabled; end: notify
};

// The worker's side of the hub's one on-window edit session. It drives a
// LiveEditSession the hub owns and reports each outcome through emit, which
// tags it with the session id. Worker-thread only; toolkit-free.
class HubEditDriver final {
public:
  using Emit = std::function<void(std::uint64_t id, OnWindowEditEvent event)>;
  // Opens (or reuses) the hub's connection. Returns it, or null with reason
  // set; started reports whether LayerD had to be launched.
  using OpenSession = std::function<LiveEditSession *(bool &started, std::string &reason)>;

  // Bounds the wait for an authenticated connection before begin_edit:
  // LayerD already listening, and LayerD launched for this session.
  static constexpr ULONGLONG await_running_ms = 2000;
  static constexpr ULONGLONG await_started_ms = 5000;
  // The connection's reconnect cadence while a session waits for LayerD.
  static constexpr DWORD reconnect_ms = 250;

  explicit HubEditDriver(Emit emit) : emit_(std::move(emit)) {}

  // Editing, or waiting for LayerD to begin: the hub keeps its connection.
  [[nodiscard]] bool live() const noexcept { return id_ != 0; }
  [[nodiscard]] bool editing() const noexcept { return id_ != 0 && editing_; }

  void apply(HubEditCommand command, LiveEditSession *session,
             const OpenSession &open);
  // Every poll of the hub's session passes through here first.
  void apply_step(const LiveEditStep &step, LiveEditSession &session);
  // The hub closed its connection (protocol fault, refused authentication).
  void connection_discarded(std::string reason);
  // How long the worker may block before the session needs a poll.
  [[nodiscard]] DWORD wait_timeout(const LiveEditSession &session) const;
  // Shutdown: tells LayerD the session ended and flushes the send once.
  void shutdown(LiveEditSession *session);

private:
  void begin(HubEditCommand command, LiveEditSession *session, const OpenSession &open);
  void finish(OnWindowEditEvent event);
  void clear() noexcept;

  Emit emit_;
  std::uint64_t id_{};
  bool editing_{};
  OnWindowEditBegin payload_;
  std::optional<std::string> pending_profile_;
};

} // namespace layerc::editor
