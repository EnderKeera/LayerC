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
#include <vector>

// The frontend's platform boundary. Reuse the toolkit-free contracts rather
// than wrapping the store or duplicating its validation and transactions.
// No native window, display, toolkit or process-handle types belong here.
namespace layerc::editor {

[[nodiscard]] std::uint32_t current_process_id() noexcept;

// The desktop's interface font description (for example "Cantarell 11"),
// when the desktop publishes one; parse_desktop_font interprets it.
[[nodiscard]] std::optional<std::string> desktop_font_description();

// Where a file chooser starts before the user has picked a folder: the home
// directory in the encoding FLTK's choosers take, or empty when unknown.
[[nodiscard]] std::string file_chooser_start_directory();

// Calls ready on the frontend loop whenever the client may hold an update;
// ready must drain take_update(). Destroy the watch before its client.
class LiveDraftWatch final {
public:
  LiveDraftWatch(const LiveDraftClient &client, std::function<void()> ready);
  ~LiveDraftWatch();
  LiveDraftWatch(const LiveDraftWatch &) = delete;
  LiveDraftWatch &operator=(const LiveDraftWatch &) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// A bounded, demand-opened scan. Matching excludes this editor's own process.
[[nodiscard]] std::vector<TaskCandidate> scan_tasks(std::string &error);
[[nodiscard]] std::optional<TaskCandidate>
matching_task(const std::vector<TaskCandidate> &tasks, const core::GameMatch &match);
[[nodiscard]] bool task_matches(const TaskCandidate &task, const core::GameMatch &match);

struct FocusedBindingCallbacks {
  std::function<void(const core::InputToken &)> record;
  std::function<void(const std::string &)> status;
  std::function<void(const std::string &)> gamepad;
  std::function<bool()> active;
};

struct FocusedKeyResult {
  bool handled{};
  std::optional<core::InputToken> token;
};

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
  [[nodiscard]] std::optional<core::InputToken> mouse_button_token() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct FolderOpenExit {
  // Empty when the launcher could not be reaped normally.
  std::optional<int> code;
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
