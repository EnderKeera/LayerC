#pragma once

#include "dialog_layout.hpp"
#include "dialog_theme.hpp"
#include "editor_state.hpp"
#include "profile_document.hpp"
#include "automation_simulator.hpp"
#include "layerc/core/automation.hpp"
#include "layerc/core/live_edit.hpp"

#include <windows.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace layerc::editor {

// The automation dialog. It edits the injected document's automation module,
// talks to the daemon only through the live-edit session, and reports every
// accepted edit through the change recorder; it has no access to its host
// window's private state.
class AutomationEditor final {
public:
  using EditReporter =
      std::function<bool(const ModuleEditOutcome &, DocumentChangeKind)>;

  AutomationEditor(ProfileDocument &document, LiveEditSession &live_edit,
                   const EditorShell &shell, const DialogTheme &theme,
                   EditReporter report_edit) noexcept;
  ~AutomationEditor();

  AutomationEditor(const AutomationEditor &) = delete;
  AutomationEditor &operator=(const AutomationEditor &) = delete;

  void open();
  [[nodiscard]] bool handle_live_message(const layerc::core::LiveMessage &message);
  void restore_live_subscription();
  [[nodiscard]] std::optional<SimulatorPanelPreview>
  panel_preview(std::string_view panel_id) const;

private:
  void update_add_enabled(HWND dialog);
  // Rejected and invalid candidate edits leave the document unchanged.
  enum class Mutation { committed, rejected, invalid };
  // Returns the rejection text for a refused edit, or nullopt once applied.
  using AutomationEdit = std::function<std::optional<std::string>(
      std::optional<layerc::core::AutomationProfile> &)>;

  void initialize(HWND dialog);
  [[nodiscard]] LRESULT handle_message(HWND, UINT, WPARAM, LPARAM);
  [[nodiscard]] bool handle_command(HWND dialog, WPARAM wparam);
  void refresh(HWND dialog);
  void refresh_nodes(HWND dialog);
  void refresh_node_properties(HWND dialog);
  void select_node(HWND dialog);
  void update_view(HWND dialog);
  void layout(HWND dialog) const;
  void apply_node_properties(HWND dialog);
  void delete_node(HWND dialog);
  void add_basic(HWND dialog);
  void add_palette_node(HWND dialog);
  void remove_selected(HWND dialog);
  [[nodiscard]] bool apply_json(HWND dialog);
  void validate_json(HWND dialog);
  void reset_simulator(HWND dialog);
  void step_simulator(HWND dialog);
  void play_simulator(HWND dialog, bool playing);
  void add_simulator_event(HWND dialog);
  void update_simulator_view(HWND dialog);
  void load_trace(HWND dialog);
  void set_trace(HWND dialog, bool enabled);
  void save_trace(HWND dialog);
  void retry_selected(HWND dialog);
  [[nodiscard]] int selected_index(HWND dialog) const;
  [[nodiscard]] const layerc::core::AutomationDefinition *selected_automation(HWND dialog) const;
  [[nodiscard]] const layerc::core::AutomationRule *selected_rule(HWND dialog) const;
  [[nodiscard]] const layerc::core::AutomationAction *selected_action(HWND dialog) const;
  friend struct AutomationEditorTestAccess;

  [[nodiscard]] Mutation mutate_automation(HWND dialog, const wchar_t *title,
                                           const AutomationEdit &edit,
                                           bool repair = false);

  ProfileDocument &document_;
  LiveEditSession &live_edit_;
  const EditorShell &shell_;
  const DialogTheme &theme_;
  EditReporter report_edit_;
  HWND dialog_{};
  DialogChrome chrome_;
  std::string trace_display_;
  LiveTraceLog trace_log_;
  AutomationSimulator simulator_;
  int node_rule_{-1};
  int node_action_{-1};
  int view_{};
  bool validation_expanded_{};
  bool validation_has_error_{};
  bool trace_enabled_{};
  bool simulator_playing_{};
  bool advanced_expression_displayed_{};
};

} // namespace layerc::editor