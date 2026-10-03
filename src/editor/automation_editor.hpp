#pragma once

#include "dialog_layout.hpp"
#include "editor_state.hpp"
#include "profile_document.hpp"
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

// Themed painting for a sub-editor dialog, provided by its host window.
class DialogTheme {
public:
  virtual void draw_list_box_item(const DRAWITEMSTRUCT &item) const = 0;
  [[nodiscard]] virtual std::optional<LRESULT>
  dialog_theme_message(HWND window, UINT message, WPARAM wparam,
                       LPARAM lparam) const = 0;

protected:
  ~DialogTheme() = default;
};

// The automation dialog. It edits the injected document's automation module,
// talks to the daemon only through the live-edit session, and reports every
// accepted edit through the change recorder; it has no access to its host
// window's private state.
class AutomationEditor final {
public:
  using ChangeRecorder = std::function<void(DocumentChangeKind)>;

  AutomationEditor(ProfileDocument &document, LiveEditSession &live_edit,
                   const EditorShell &shell, const DialogTheme &theme,
                   ChangeRecorder record_change) noexcept;
  ~AutomationEditor();

  AutomationEditor(const AutomationEditor &) = delete;
  AutomationEditor &operator=(const AutomationEditor &) = delete;

  void open();
  [[nodiscard]] bool handle_live_message(const layerc::core::LiveMessage &message);
  void restore_live_subscription();

private:
  void update_add_enabled(HWND dialog);
  // Outcome of mutate_automation; rejected and invalid edits both restore the
  // pre-edit snapshot.
  enum class Mutation { committed, rejected, invalid };
  // Returns the rejection text for a refused edit, or nullopt once applied.
  using AutomationEdit = std::function<std::optional<std::wstring>()>;

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
  [[nodiscard]] layerc::core::AutomationDefinition *selected_automation(HWND dialog) const;
  [[nodiscard]] layerc::core::AutomationRule *selected_rule(HWND dialog) const;
  [[nodiscard]] layerc::core::AutomationAction *selected_action(HWND dialog) const;
  [[nodiscard]] Mutation mutate_automation(HWND dialog, const wchar_t *title,
                                           const AutomationEdit &edit);

  ProfileDocument &document_;
  LiveEditSession &live_edit_;
  const EditorShell &shell_;
  const DialogTheme &theme_;
  ChangeRecorder record_change_;
  HWND dialog_{};
  DialogChrome chrome_;
  std::string trace_json_;
  std::string trace_display_;
  std::vector<layerc::core::AutomationEvent> trace_events_;
  std::vector<layerc::core::AutomationEvent> simulator_events_;
  std::unique_ptr<layerc::core::AutomationRuntime> simulator_;
  std::size_t simulator_index_{};
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