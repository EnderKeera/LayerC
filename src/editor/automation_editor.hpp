#pragma once

#include "dialog_layout.hpp"
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

class AutomationHost {
public:
  virtual ~AutomationHost() = default;

  [[nodiscard]] virtual ProfileDocument &document() noexcept = 0;
  virtual void mark_changed(DocumentChangeKind kind) = 0;
  [[nodiscard]] virtual HINSTANCE instance() const noexcept = 0;
  [[nodiscard]] virtual HWND owner_window() const noexcept = 0;
  [[nodiscard]] virtual HFONT owner_font() const noexcept = 0;
  [[nodiscard]] virtual bool capture_exclusion_enabled() const noexcept = 0;
  virtual void draw_owner_list_item(const DRAWITEMSTRUCT &item) const = 0;
  [[nodiscard]] virtual std::optional<LRESULT>
  owner_theme_message(HWND window, UINT message, WPARAM wparam,
                      LPARAM lparam) const = 0;
  [[nodiscard]] virtual bool live_connected() const noexcept = 0;
  [[nodiscard]] virtual bool live_authenticated() const noexcept = 0;
  virtual void advance_live_revision() noexcept = 0;
  [[nodiscard]] virtual layerc::core::ProfileRevision
  live_revision() const noexcept = 0;
  virtual void send_live(const layerc::core::LiveMessage &message) = 0;
};

class AutomationEditor final {
public:
  explicit AutomationEditor(AutomationHost &owner) noexcept;
  ~AutomationEditor();

  AutomationEditor(const AutomationEditor &) = delete;
  AutomationEditor &operator=(const AutomationEditor &) = delete;

  void open();
  [[nodiscard]] bool handle_live_message(const layerc::core::LiveMessage &message);
  void restore_live_subscription();

private:
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

  AutomationHost &owner_;
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