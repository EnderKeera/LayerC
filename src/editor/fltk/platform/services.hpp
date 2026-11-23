#pragma once

#include "automation_trace_client.hpp" // IWYU pragma: export
#include "binding_capture.hpp" // IWYU pragma: export
#include "daemon_link.hpp" // IWYU pragma: export
#include "editor_appearance.hpp" // IWYU pragma: export
#include "live_draft_client.hpp" // IWYU pragma: export
#include "task_list.hpp" // IWYU pragma: export
#include "layerc/platform/profile_store.hpp" // IWYU pragma: export
#include "layerc/platform/profile_visual_assets.hpp" // IWYU pragma: export

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The frontend's platform boundary. Reuse the toolkit-free contracts rather
// than wrapping the store or duplicating its validation and transactions.
// No native window, display, toolkit or process-handle types belong here.
namespace layerc::editor {

#ifdef _WIN32
inline constexpr bool windows_editor_keys = true;
#else
inline constexpr bool windows_editor_keys = false;
#endif

[[nodiscard]] std::uint32_t current_process_id() noexcept;

// The desktop's interface font description (for example "Cantarell 11"),
// when the desktop publishes one; parse_desktop_font interprets it.
[[nodiscard]] std::optional<std::string> desktop_font_description();

// Where a file chooser starts before the user has picked a folder: the home
// directory in the encoding FLTK's choosers take, or empty when unknown.
[[nodiscard]] std::string file_chooser_start_directory();
[[nodiscard]] std::string profile_directory_unavailable_reason();

struct StandaloneLiveDraftsUnavailable {
  std::string reason;
};

class StandaloneLiveDrafts {
public:
  virtual ~StandaloneLiveDrafts() = default;
  [[nodiscard]] virtual std::uint64_t publish(std::string profile_id,
                                               std::string profile_json) = 0;
  virtual void end() = 0;
  [[nodiscard]] virtual std::optional<LiveDraftUpdate> take_update() = 0;
};

using StandaloneLiveDraftsResult =
    std::variant<std::unique_ptr<StandaloneLiveDrafts>, StandaloneLiveDraftsUnavailable>;

// Linux uses a standalone daemon lease. Other platforms report an explicit
// unavailable result until their edit-session service is wired.
[[nodiscard]] bool standalone_live_drafts() noexcept;
[[nodiscard]] StandaloneLiveDraftsResult
make_standalone_live_drafts(std::function<void()> ready);

[[nodiscard]] std::string_view folder_opener_name() noexcept;

// A bounded, demand-opened scan. Matching excludes this editor's own process.
[[nodiscard]] std::vector<TaskCandidate> scan_tasks(std::string &error);
[[nodiscard]] std::optional<TaskCandidate>
matching_task(const std::vector<TaskCandidate> &tasks, const core::GameMatch &match);
[[nodiscard]] bool task_matches(const TaskCandidate &task, const core::GameMatch &match);
[[nodiscard]] bool window_picking_supported() noexcept;
// Physical screen pixels, not FLTK's scaled event coordinates.
[[nodiscard]] std::optional<TaskCandidate> task_at_screen_point(int x, int y);
[[nodiscard]] std::optional<TaskCandidate> task_under_cursor();

class PickCaptureWatch {
public:
  virtual ~PickCaptureWatch() = default;
  // FLTK's grab is void; check that the native owner actually took capture.
  [[nodiscard]] virtual bool owns_capture() const noexcept = 0;
};
struct PickCaptureTimer {
  using Callback = void (*)(void *);
  void (*schedule)(double, Callback, void *);
  void (*cancel)(Callback, void *);
};
[[nodiscard]] std::unique_ptr<PickCaptureWatch>
watch_pick_capture(std::uintptr_t owner, PickCaptureTimer timer, std::function<void()> lost);
// Ends Pick window's FLTK grab. Native capture is released only while `owner`
// still holds it; false when another window of this thread has taken capture
// since, which then keeps it.
[[nodiscard]] bool release_pick_grab(std::uintptr_t owner);

struct FocusedBindingCallbacks {
  std::function<void(const core::InputToken &)> record;
  std::function<void(const std::string &)> status;
  std::function<void(const std::string &)> gamepad;
  std::function<bool()> active;
};

struct FocusedKeyResult {
  // Ignore consumes the dialog event without recording or showing an error
  // (for example, IME composition or text delivery, not a physical key).
  enum class Kind { unhandled, ignore, cancel, key };
  Kind kind{};
  std::optional<core::InputToken> token;
};

[[nodiscard]] std::string_view focused_binding_hid_note() noexcept;

// Passive, dialog-lifetime listeners. The platform adapter integrates its
// owned notifications with the frontend loop, without exposing native types.
class FocusedBindingEvents final {
public:
  explicit FocusedBindingEvents(FocusedBindingCallbacks callbacks);
  ~FocusedBindingEvents();
  FocusedBindingEvents(const FocusedBindingEvents &) = delete;
  FocusedBindingEvents &operator=(const FocusedBindingEvents &) = delete;
  void start();
  void stop();
  [[nodiscard]] FocusedKeyResult keyboard_token();
  [[nodiscard]] bool mouse_input_inside(int x, int y, int w, int h) const;
  [[nodiscard]] std::optional<core::InputToken> mouse_button_token() const;
  [[nodiscard]] std::optional<core::InputToken> mouse_wheel_token() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct FolderOpenExit {
  // Empty when no child exists or its exit status cannot be read.
  std::optional<int> code;
  bool accepted_without_process{};
};

// The external file manager starts in its own session. Closing the editor
// never signals it; stop() makes the same bounded reap attempt as before.
class FolderOpener final {
public:
  FolderOpener();
  ~FolderOpener();
  FolderOpener(const FolderOpener &) = delete;
  FolderOpener &operator=(const FolderOpener &) = delete;
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] bool start(const std::filesystem::path &directory);
  [[nodiscard]] std::optional<FolderOpenExit> poll();
  void stop() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The captured-frame service contract lives in platform/calibration.hpp.
class CalibrationSource;

} // namespace layerc::editor
