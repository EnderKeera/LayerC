#pragma once

#include "layerc/core/automation.hpp"
#include "layerc/core/live_edit.hpp"

#include <windows.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace layerc::editor {

class EditorWindow;

class AutomationEditor final {
public:
  explicit AutomationEditor(EditorWindow &owner) noexcept;
  ~AutomationEditor();

  AutomationEditor(const AutomationEditor &) = delete;
  AutomationEditor &operator=(const AutomationEditor &) = delete;

  void open();
  [[nodiscard]] bool handle_live_message(const layerc::core::LiveMessage &message);
  void restore_live_subscription();

private:
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

  EditorWindow &owner_;
  HWND dialog_{};
  HFONT font_{};
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
};

} // namespace layerc::editor