#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The on-window edit session: LayerD draws the profile's bars over the game
// window while the editor drags them, and may let the user drag them in game
// (Direct Drag). Toolkit-free; no native window or handle types.
namespace layerc::editor {

// Whether this platform edits bars on a game window through LayerD's edit
// session. Linux has no such channel: positioning publishes the live draft.
[[nodiscard]] bool on_window_edit_sessions() noexcept;

struct OnWindowEditBegin {
  // LayerD's canonical key for the saved profile file.
  std::string profile_key;
  std::string panel_id;
  // The serialized document the session starts from.
  std::string profile_json;
};

struct OnWindowEditEvent {
  enum class Kind : std::uint8_t {
    // LayerD is starting or not yet connected; begin follows when it answers.
    awaiting,
    started,
    // LayerD confirmed the session; Direct Drag may be offered.
    acknowledged,
    // LayerD moved a bar during Direct Drag, already validated and ordered.
    anchor,
    ended,
    // LayerD never exposed its live-edit service before the deadline.
    expired,
    protocol_error,
  };
  enum class EndReason : std::uint8_t {
    connection_lost,
    daemon_error,
    daemon_ended,
    target_closed,
    // The session could not start; text names why.
    unavailable,
  };

  Kind kind{Kind::ended};
  EndReason reason{EndReason::daemon_ended};
  std::string panel_id{};
  double x{};
  double y{};
  bool final{};
  // LayerD's reason, the protocol error, or the unavailable reason.
  std::string text{};

  bool operator==(const OnWindowEditEvent &) const = default;
};

// One session at a time. Calls never wait for LayerD; results arrive as
// events, and `ready` (given at creation) runs on the UI loop when some wait.
// Destroying the object ends its session.
class OnWindowEdit {
public:
  virtual ~OnWindowEdit() = default;
  // Starts editing on the TaskCandidate window, replacing an older session.
  virtual void begin(std::uint64_t window, OnWindowEditBegin payload) = 0;
  // The document after an accepted edit; consecutive calls coalesce.
  virtual void live_profile(std::string profile_json) = 0;
  // A preview drag; final marks its release.
  virtual void anchor(std::string panel_id, double x, double y, bool final) = 0;
  virtual void direct_drag(bool enabled) = 0;
  // Ends the session, telling LayerD when notify is set.
  virtual void end(bool notify) = 0;
  [[nodiscard]] virtual std::vector<OnWindowEditEvent> take_events() = 0;
};

// Null where on_window_edit_sessions() is false or the link is unavailable.
[[nodiscard]] std::unique_ptr<OnWindowEdit>
make_on_window_edit(std::function<void()> ready);

} // namespace layerc::editor
