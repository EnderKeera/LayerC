#include "editor_app.hpp"
#include "diagnostics_window.hpp"
#include "tasks_window.hpp"
#include "profile_edit.hpp"
#include "profile_mutations.hpp"
#include "binding_capture.hpp"
#include "platform/product_text.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Tabs.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <limits>
#include <utility>

namespace layerc::editor {
namespace core = layerc::core;

std::optional<std::size_t> EditorApp::cooldown_index() const {
  if (!session.has_document()) return std::nullopt;
  const auto &items = session.document().profile().cooldowns;
  for (std::size_t i = 0; i < items.size(); ++i)
    if (items[i].id == cooldown_id_) return i;
  return std::nullopt;
}
std::optional<std::size_t> EditorApp::panel_index() const {
  if (!session.has_document()) return std::nullopt;
  const auto &items = session.document().profile().panels;
  for (std::size_t i = 0; i < items.size(); ++i)
    if (items[i].id == panel_id_) return i;
  return std::nullopt;
}
std::string editor_window_title(const std::optional<std::string_view> profile_name,
                                const bool dirty) {
  std::string title = "LayerE — ";
  title += editor_product_line();
  if (profile_name) {
    title += " — ";
    title += *profile_name;
    if (dirty) title += " *";
  }
  return title;
}

void EditorApp::report(const StatusKind kind, std::string text, const StatusVoice voice) {
  status_text_ = std::move(text);
  status_kind_ = kind;
  render_status();
  const auto title = session.has_document()
      ? editor_window_title(session.document().profile().name,
                            pending_fields_ || session.has_unsaved_changes())
      : editor_window_title();
  window.copy_label(title.c_str());
  status_pane_->set_history(session.can_undo() || pending_fields_,
                            session.can_redo() && !pending_fields_);
  if (menus) menus->update();
  // The outcome of an explicit action and every failure are spoken; an edit's
  // own status line is not (DesignSystem.md, Screen readers).
  if (accessibility && status_is_announced(last_announced_, status_text_, voice)) {
    last_announced_ = status_text_;
    accessibility->announce(nullptr, last_announced_);
  }
}
void EditorApp::set_capture_exclusion_warning(const std::string_view warning) {
  if (capture_exclusion_warning_ == warning) return;
  capture_exclusion_warning_ = warning;
  render_status();
  if (accessibility && !warning.empty())
    accessibility->announce(nullptr, capture_exclusion_warning_);
}

void EditorApp::render_status() {
  if (capture_exclusion_warning_.empty()) {
    status_pane_->set_status(status_kind_, status_text_);
    return;
  }
  // Keep the privacy fault visible without discarding unrelated action outcomes.
  const auto kind = status_kind_ == StatusKind::failure ? status_kind_ : StatusKind::warning;
  status_pane_->set_status(kind, capture_exclusion_warning_ + "\n" + status_text_);
}

void EditorApp::query_status() {
  const auto state = query_daemon_status();
  daemon_text_ = state.summary();
  daemon_kind_ = state.kind();
  for (const auto &capability : state.capabilities) {
    daemon_text_ += "\n" + capability.name + ": " +
                    std::string(layerc::runtime::availability_name(capability.availability));
    if (!capability.reasons.empty()) daemon_text_ += " (" + capability.reasons + ")";
  }
  update_daemon_status();
}
void EditorApp::refresh() {
  pending_fields_ = false;
  last_field_ = nullptr;
  fill_lists();
  refresh_forms();
  update_position_button();
  preview->redraw();
}

bool EditorApp::apply_fields(const bool text_change) {
  if (const auto error = apply_field_buffers(text_change)) {
    // Repeated on every keystroke while the field stays invalid.
    report(StatusKind::failure, *error, StatusVoice::changed);
    return false;
  }
  return true;
}

std::optional<std::string> EditorApp::apply_field_buffers(const bool text_change) {
  if (!session.has_document()) return std::nullopt;
  if (automation && active_tab_ == automation->group())
    return automation->apply_pending();
  auto candidate = session.document().profile();
  std::string parse_error;
  if (active_tab_ == profile_tab) {
    profile_page_->read(candidate);
  } else if (active_tab_ == cooldown_tab) {
    if (const auto index = cooldown_index())
      parse_error = cooldown_page_->read(candidate.cooldowns[*index]).value_or("");
  } else if (active_tab_ == panel_tab) {
    if (const auto index = panel_index())
      parse_error = panel_page_->read(candidate.panels[*index]).value_or("");
  }
  std::optional<std::string> error;
  if (!parse_error.empty()) error = std::move(parse_error);
  else error = session.edit([&](auto &profile) { profile = std::move(candidate); }, text_change);
  pending_fields_ = error.has_value();
  if (error) return "Invalid input: " + *error;
  return std::nullopt;
}

void EditorApp::module_fields_changed(const bool pending) {
  pending_fields_ = pending;
  last_field_ = nullptr;
  report(StatusKind::information, pending ? "Pending automation fields. Autosave validates them." : edit_status(),
         StatusVoice::silent);
  if (pending) schedule_autosave(true);
}
void EditorApp::field_changed(Fl_Widget *widget) {
  if (widget != last_field_) session.end_text_edit();
  last_field_ = widget;
  if (apply_fields(dynamic_cast<Fl_Input *>(widget) != nullptr)) {
    fill_lists();
    report(StatusKind::information, edit_status(), StatusVoice::silent);
    preview->redraw();
  }
  // Follows the typed colours even while another field is still invalid.
  if (active_tab_ == panel_tab) panel_page_->update_contrast();
}
void EditorApp::selection_changed(Fl_Widget *widget) {
  const auto target = static_cast<Fl_Hold_Browser *>(widget)->value();
  if (target <= 0) return;
  if (widget == profiles) {
    const auto index = static_cast<std::size_t>(target - 1);
    if (index >= session.profiles().profiles.size()) return;
    const auto path = session.profiles().profiles[index].path;
    if (session.has_document() && path == session.document().path()) return;
    if (!may_leave()) { fill_lists(); return; }
    if (const auto error = session.open_path(path)) { report(StatusKind::failure, *error); fill_lists(); return; }
    cooldown_id_.clear(); panel_id_.clear(); binding_index_ = -1;
    refresh();
    report(StatusKind::success, "Profile loaded. Changes autosave; new profiles need Save once.");
    return;
  }
  if (!apply_fields()) { fill_lists(); return; }
  session.end_text_edit();
  last_field_ = nullptr;
  const auto index = static_cast<std::size_t>(target - 1);
  const auto &profile = session.document().profile();
  if (cooldown_page_->owns_list(widget) && index < profile.cooldowns.size())
    cooldown_id_ = profile.cooldowns[index].id;
  if (panel_page_->owns_list(widget) && index < profile.panels.size())
    panel_id_ = profile.panels[index].id;
  if (binding_page_->owns_list(widget) && index < profile.bindings.size())
    binding_index_ = target - 1;
  refresh_forms();
  preview->redraw();
  menus->update();
}
void EditorApp::tab_changed() {
  auto *next = tabs->value();
  if (next == active_tab_) return;
  if (!apply_fields()) { tabs->value(active_tab_); return; }
  session.end_text_edit();
  last_field_ = nullptr;
  active_tab_ = static_cast<Fl_Group *>(next);
  refresh_forms();
  menus->update();
}
bool EditorApp::save() {
  if (!session.has_document()) { report(StatusKind::failure, "No profile is open."); return false; }
  if (saving_) return false;
  autosave_timer_.cancel();
  saving_ = true;
  if (const auto error = apply_field_buffers()) {
    saving_ = false;
    observed_generation_ = session.edit_generation();
    report(StatusKind::failure, "Not saved. " + *error);
    return false;
  }
  session.end_text_edit();
  const auto outcome = session.save([&] {
    return session.document().bundle().items_upgrade_needed
        ? confirm_items_profile_upgrade() : confirm_profile_upgrade();
  });
  saving_ = false;
  observed_generation_ = session.edit_generation();
  if (outcome.saved()) { stop_edit_services(); refresh(); }
  report(outcome.kind, outcome.text);
  query_status();
  return outcome.saved();
}
void EditorApp::close_window() {
  if (!may_leave()) return;
  end_on_window(true);
  stop_edit_services();
  if (diagnostics) diagnostics->hide();
  if (tasks_window) tasks_window->hide();
  window.hide();
}

bool EditorApp::may_leave() {
  if (!pending_fields_ && !session.has_unsaved_changes()) return true;
  if (!session.is_draft() && flush_autosave()) return true;
  autosave_timer_.cancel();
  fl_message_title("Unsaved changes");
  const auto decision = fl_choice("This profile has unsaved changes.", "Cancel", "Discard", "Save");
  if (decision == 2) return save();
  if (decision != 1) return false;
  if (session.is_draft()) session.close_document();
  else if (const auto error = session.open_path(session.document().path())) {
    report(StatusKind::failure, "Could not discard safely: " + *error); return false;
  }
  refresh();
  return true;
}
void EditorApp::changed(const std::optional<std::string> &error) {
  if (error) { report(StatusKind::failure, "Not changed: " + *error); return; }
  refresh();
  report(StatusKind::information, edit_status(), StatusVoice::silent);
}

bool EditorApp::command_enabled(const EditorCommand action) const {
  return layerc::editor::command_enabled(*this, action);
}

void EditorApp::command(const EditorCommand action) {
  // Buttons keep their own guards and messages; menus, shortcuts and context
  // menus go through run_command, which checks enablement first.
  command_spec(action).run(*this);
}

bool confirm_items_profile_upgrade() {
  fl_message_title("Upgrade profile");
  return fl_choice("Saving adds overlay items or active durations (profile schema 5).\n"
                   "LayerC versions before 2.7 cannot open the saved profile.",
                   "Cancel", "Upgrade", nullptr) == 1;
}
bool confirm_profile_upgrade() {
  fl_message_title("Upgrade profile");
  return fl_choice("Upgrade this legacy profile to the current format?\n"
                   "Older LayerC versions may no longer read it.",
                   "Cancel", "Upgrade", nullptr) == 1;
}
} // namespace layerc::editor
