#pragma once

#include "editor_state.hpp"
#include "profile_document.hpp"
#include "layerc/core/types.hpp"
#include "layerc/platform/window_query.hpp"

#include <windows.h>

#include <functional>
#include <string>
#include <string_view>

namespace layerc::editor {

// The selected cooldown's visual trigger: its summary controls, enablement,
// removal, and calibration. It edits the injected document's visual module
// and commits through the host's one commit pipeline; it reads the live-edit
// target and drives the task picker only through the injected references.
class VisualEditor final {
public:
  struct Host {
    std::function<const layerc::core::CooldownDefinition *()>
        selected_cooldown;
    // Commits the open document through the shared pipeline.
    std::function<CommitResult()> commit_document;
    // Records a completed commit (dirty state, timers, notification).
    std::function<void()> mark_committed;
    std::function<std::wstring()> write_failure_message;
    // Saves a new draft once and flushes pending edits; false to abort.
    std::function<bool(const wchar_t *first_save_message)>
        require_stored_profile;
    // Shows the task picker so the user can choose a target window.
    std::function<void()> show_tasks;
  };

  VisualEditor(ProfileDocument &document, EditorShell &shell,
               const LiveEditSession &live_edit, TaskPicker &task_picker,
               Host host) noexcept;

  VisualEditor(const VisualEditor &) = delete;
  VisualEditor &operator=(const VisualEditor &) = delete;

  void refresh();
  void set_enabled(bool enabled);
  void remove_assignment();
  // Calibrates on the live-edit target when it matches, else asks the user
  // to pick a matching task.
  void calibrate();
  // Calibrates on a picked task after checking that it matches.
  void calibrate_task(const layerc::platform::WindowCandidate &candidate);
  bool launch_calibration(HWND target);

private:
  [[nodiscard]] bool merge_calibration_result(std::string_view visual_json);
  [[nodiscard]] bool save();
  void set_status(std::wstring_view text) const;

  ProfileDocument &document_;
  EditorShell &shell_;
  const LiveEditSession &live_edit_;
  TaskPicker &task_picker_;
  Host host_;
};

} // namespace layerc::editor
