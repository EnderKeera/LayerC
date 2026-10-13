#include "editor_app.hpp"
#include "profile_edit.hpp"
#include "profile_mutations.hpp"
#include "binding_capture.hpp"

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
namespace {
bool confirm_upgrade() {
  return fl_choice("Upgrade this legacy profile to the current format?\n"
                   "Older LayerC versions may no longer read it.",
                   "Cancel", "Upgrade", nullptr) == 1;
}
}

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
void EditorApp::report(std::string text) {
  status_text_ = std::move(text);
  status->value(status_text_.c_str());
  status->redraw();
  title_text_ = "LayerE — Linux X11 editor";
  if (session.has_document()) {
    title_text_ += " — " + session.document().profile().name;
    if (pending_fields_ || session.has_unsaved_changes()) title_text_ += " *";
  }
  window.copy_label(title_text_.c_str());
  if (session.can_undo() || pending_fields_) undo_button->activate();
  else undo_button->deactivate();
  if (session.can_redo() && !pending_fields_) redo_button->activate();
  else redo_button->deactivate();
  if (menus) menus->update();
  sync_edit_services();
}
void EditorApp::query_status() {
  const auto state = query_daemon_status();
  daemon_text_ = state.summary();
  for (const auto &capability : state.capabilities) {
    daemon_text_ += "\n" + capability.name + ": " + capability.availability;
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
  if (!session.has_document()) return true;
  if (automation && active_tab_ == automation->group()) return automation->apply_pending();
  auto candidate = session.document().profile();
  std::string parse_error;
  if (active_tab_ == profile_tab) {
    candidate.name = name->value();
    candidate.game.executable_name = executable->value();
    candidate.game.executable_path = executable_path->value();
    candidate.game.window_class = window_class->value();
    candidate.game.window_title = window_title->value();
    candidate.enabled = enabled->value() != 0;
  } else if (active_tab_ == cooldown_tab) {
    if (const auto index = cooldown_index()) {
      const auto seconds = parse_duration_seconds(duration->value());
      const auto count = parse_count(charges->value(), core::min_cooldown_charges,
                                      core::max_cooldown_charges);
      if (!seconds || !count) parse_error = "Duration or charges is outside its supported range.";
      else {
        auto &item = candidate.cooldowns[*index];
        item.name = cooldown_name->value();
        item.duration = *seconds;
        item.max_charges = *count;
        item.recovery = static_cast<core::RecoveryMode>(recovery->value());
        item.retrigger = static_cast<core::RetriggerPolicy>(retrigger->value());
      }
    }
  } else if (active_tab_ == panel_tab) {
    if (const auto index = panel_index()) {
      const auto x = parse_anchor(anchor_x->value()), y = parse_anchor(anchor_y->value());
      const auto width = parse_count(bar_width->value(), core::min_bar_width, core::max_bar_width);
      const auto height = parse_count(bar_height->value(), core::min_bar_height, core::max_bar_height);
      const auto fg = parse_rgb(fill->value()), bg = parse_rgb(background->value());
      const auto edge = parse_rgb(border->value()), text = parse_rgb(text_color->value());
      if (!x || !y || !width || !height || !fg || !bg || !edge || !text)
        parse_error = "Use anchors 0–1, supported bar dimensions, and #RRGGBB colors.";
      else {
        auto &item = candidate.panels[*index];
        item.name = panel_name->value();
        item.anchor_x = *x; item.anchor_y = *y;
        item.layout = static_cast<core::PanelLayout>(layout->value());
        item.style = static_cast<core::PanelStyle>(style->value());
        item.bar.width = *width; item.bar.height = *height;
        item.bar.fill_color = *fg; item.bar.background_color = *bg;
        item.bar.border_color = *edge; item.bar.text_color = *text;
        item.bar.show_text = show_text->value() != 0;
        item.bar.hide_when_ready = hide_ready->value() != 0;
      }
    }
  }
  std::optional<std::string> error;
  if (!parse_error.empty()) error = std::move(parse_error);
  else error = session.edit([&](auto &profile) { profile = std::move(candidate); }, text_change);
  pending_fields_ = error.has_value();
  if (error) report("Invalid input: " + *error);
  return !error;
}

void EditorApp::module_fields_changed(const bool pending) {
  pending_fields_ = pending;
  last_field_ = nullptr;
  report(pending ? "Pending automation fields. Autosave validates them." : edit_status());
  if (pending) schedule_autosave(true);
}
void EditorApp::field_changed(Fl_Widget *widget) {
  if (widget != last_field_) session.end_text_edit();
  last_field_ = widget;
  if (widget == member) {
    // Navigation must not discard a locally invalid panel field buffer.
    if (!apply_fields()) { member->value(member_index_); return; }
    session.end_text_edit();
    member_index_ = member->value();
    refresh_forms();
    return;
  }
  if (widget == member_enabled) {
    if (!apply_fields()) return;
    const auto index = member->value();
    const auto &items = session.document().profile().cooldowns;
    if (index >= 0 && static_cast<std::size_t>(index) < items.size()) {
      const auto id = items[static_cast<std::size_t>(index)].id;
      changed(session.edit([&](auto &profile) {
        (void)set_panel_member(profile, panel_id_, id, member_enabled->value() != 0);
      }));
    }
    return;
  }
  if (apply_fields(dynamic_cast<Fl_Input *>(widget) != nullptr)) {
    fill_lists();
    report(edit_status());
    preview->redraw();
  }
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
    if (const auto error = session.open_path(path)) { report(*error); fill_lists(); return; }
    cooldown_id_.clear(); panel_id_.clear(); binding_index_ = -1;
    refresh();
    report("Profile loaded. Changes autosave; new profiles need Save once.");
    return;
  }
  if (!apply_fields()) { fill_lists(); return; }
  session.end_text_edit();
  last_field_ = nullptr;
  const auto index = static_cast<std::size_t>(target - 1);
  const auto &profile = session.document().profile();
  if (widget == cooldowns && index < profile.cooldowns.size()) cooldown_id_ = profile.cooldowns[index].id;
  if (widget == panels && index < profile.panels.size()) panel_id_ = profile.panels[index].id;
  if (widget == bindings && index < profile.bindings.size()) binding_index_ = target - 1;
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
  if (!session.has_document()) { report("No profile is open."); return false; }
  if (saving_) return false;
  Fl::remove_timeout(autosave_tick, this);
  saving_ = true;
  if (!apply_fields()) {
    saving_ = false;
    observed_generation_ = session.edit_generation();
    // Keep the field's own error, marked so a refused Save is distinguishable.
    if (!status_text_.starts_with("Not saved")) report("Not saved. " + status_text_);
    return false;
  }
  session.end_text_edit();
  const auto outcome = session.save(confirm_upgrade);
  saving_ = false;
  observed_generation_ = session.edit_generation();
  if (outcome.saved()) { stop_edit_services(); refresh(); }
  report(outcome.text);
  query_status();
  return outcome.saved();
}
bool EditorApp::may_leave() {
  if (!pending_fields_ && !session.has_unsaved_changes()) return true;
  if (!session.is_draft() && flush_autosave()) return true;
  Fl::remove_timeout(autosave_tick, this);
  const auto decision = fl_choice("This profile has unsaved changes.", "Cancel", "Discard", "Save");
  if (decision == 2) return save();
  if (decision != 1) return false;
  if (session.is_draft()) session.close_document();
  else if (const auto error = session.open_path(session.document().path())) {
    report("Could not discard safely: " + *error); return false;
  }
  refresh();
  return true;
}
void EditorApp::changed(const std::optional<std::string> &error) {
  if (error) { report("Not changed: " + *error); return; }
  refresh();
  report(edit_status());
}

bool EditorApp::command_enabled(const EditorCommand action) const {
  switch (action) {
  case EditorCommand::new_profile:
  case EditorCommand::import_profiles:
  case EditorCommand::refresh:
  case EditorCommand::profile_page:
  case EditorCommand::cooldown_page:
  case EditorCommand::binding_page:
  case EditorCommand::panel_page:
  case EditorCommand::automation_page:
  case EditorCommand::visual_page:
  case EditorCommand::diagnostics:
  case EditorCommand::shortcuts_help:
  case EditorCommand::about:
  case EditorCommand::options:
  case EditorCommand::appearance:
  case EditorCommand::open_folder:
  case EditorCommand::tasks:
  case EditorCommand::close: return true;
  case EditorCommand::duplicate_cooldown: return cooldown_index().has_value();
  case EditorCommand::panel_colors: return panel_index().has_value();
  case EditorCommand::undo: return session.has_document() && (pending_fields_ || session.can_undo());
  case EditorCommand::redo: return session.has_document() && !pending_fields_ && session.can_redo();
  case EditorCommand::toggle_enabled:
  case EditorCommand::delete_profile: return session.has_document() && session.current_row().has_value();
  case EditorCommand::delete_cooldown: return cooldown_index().has_value();
  case EditorCommand::delete_panel: return panel_index().has_value();
  case EditorCommand::delete_binding:
    return session.has_document() && binding_index_ >= 0 &&
        static_cast<std::size_t>(binding_index_) < session.document().profile().bindings.size();
  case EditorCommand::add_keyboard:
  case EditorCommand::add_mouse:
  case EditorCommand::capture_binding:
    return session.has_document() && !session.document().profile().cooldowns.empty();
  default: return session.has_document();
  }
}

void EditorApp::command(const EditorCommand action) {
  if (action == EditorCommand::close) {
    if (may_leave()) { stop_edit_services(); window.hide(); }
    return;
  }
  Fl_Group *page = nullptr;
  switch (action) {
  case EditorCommand::profile_page: page = profile_tab; break;
  case EditorCommand::cooldown_page: page = cooldown_tab; break;
  case EditorCommand::binding_page: page = binding_tab; break;
  case EditorCommand::panel_page: page = panel_tab; break;
  case EditorCommand::automation_page: if (automation) page = automation->group(); break;
  case EditorCommand::visual_page: if (visual) page = visual->group(); break;
  default: break;
  }
  if (page) { tabs->value(page); tab_changed(); tabs->redraw(); return; }
  if (action == EditorCommand::save) { (void)save(); return; }
  if (tools_command(action)) return;
  if (action == EditorCommand::tasks) { show_tasks(); return; }
  if (action == EditorCommand::position) { position_command(); return; }
  if (action == EditorCommand::diagnostics) {
    if (!diagnostics) diagnostics = std::make_unique<DiagnosticsWindow>();
    diagnostics->show();
    return;
  }
  if (action == EditorCommand::shortcuts_help) {
    show_text_dialog("Keyboard shortcuts — LayerE", EditorMenus::shortcut_help());
    return;
  }
  if (action == EditorCommand::about) {
    show_about_dialog(daemon_text_, accessibility_status && accessibility_status->label()
                                        ? accessibility_status->label() : "");
    return;
  }
  if (action == EditorCommand::refresh) { query_status(); return; }
  if (action == EditorCommand::new_profile) {
    if (!may_leave()) return;
    session.new_profile(); cooldown_id_.clear(); panel_id_.clear();
    active_tab_ = profile_tab; tabs->value(profile_tab);
    refresh(); report("New unsaved profile. Enter an executable name or path."); return;
  }
  if (action == EditorCommand::import_profiles) { import_profiles(); return; }
  if (action == EditorCommand::export_json || action == EditorCommand::export_pack) {
    export_profile(action == EditorCommand::export_pack); return;
  }
  if (!session.has_document()) { report("Create or select a profile first."); return; }
  if (action == EditorCommand::undo || action == EditorCommand::redo) {
    if (pending_fields_) { refresh(); report("Invalid field edit discarded."); return; }
    session.end_text_edit();
    const bool done = action == EditorCommand::undo ? session.undo() : session.redo();
    refresh(); report(done ? "History restored. " + edit_status() : "No history in that direction."); return;
  }
  if (action == EditorCommand::rename) {
    // F2 renames the selected cooldown on its page, otherwise the profile.
    const bool cooldown = active_tab_ == cooldown_tab && cooldown_index();
    if (!cooldown) { tabs->value(profile_tab); tab_changed(); tabs->redraw(); }
    if (active_tab_ != (cooldown ? cooldown_tab : profile_tab)) return; // invalid field kept
    auto *field = cooldown ? cooldown_name : name;
    field->take_focus();
    field->insert_position(field->size(), 0);
    return;
  }
  if (action == EditorCommand::duplicate) {
    if (!may_leave()) return;
    changed(session.duplicate_current()); return;
  }
  if (action == EditorCommand::delete_profile) {
    if (!may_leave()) return;
    if (!session.has_document()) { report("Draft discarded."); return; }
    if (fl_choice("Delete this profile, all modules and recovery copies?\nThis cannot be undone.",
                  "Cancel", "Delete", nullptr) != 1) return;
    const auto outcome = session.delete_current();
    refresh(); report(outcome.text); query_status(); return;
  }
  if (action == EditorCommand::toggle_enabled) {
    if (!may_leave()) return;
    const auto row = session.current_row();
    if (!row) { report("Save this draft before toggling it on disk."); return; }
    const auto outcome = session.set_enabled(*row, !session.document().profile().enabled, confirm_upgrade);
    refresh(); report(outcome.text); query_status(); return;
  }
  if (!apply_fields()) return;
  session.end_text_edit();
  switch (action) {
  case EditorCommand::add_cooldown: {
    std::string id;
    const auto error = session.edit([&](auto &profile) { id = add_cooldown(profile); });
    if (!error) cooldown_id_ = std::move(id);
    changed(error); break;
  }
  case EditorCommand::duplicate_cooldown: {
    std::string id;
    const auto error = session.edit([&](auto &profile) { id = duplicate_cooldown(profile, cooldown_id_); });
    if (!error && id.empty()) { report("Select a cooldown to duplicate."); break; }
    if (!error) cooldown_id_ = std::move(id);
    changed(error);
    if (!error) report("Cooldown duplicated without bindings; add a binding for the copy.");
    break;
  }
  case EditorCommand::delete_cooldown:
    if (fl_choice("Remove this cooldown, its bindings and memberships?", "Cancel", "Remove", nullptr) == 1)
      changed(session.remove_cooldown(cooldown_id_));
    break;
  case EditorCommand::add_panel: {
    std::string id;
    const auto error = session.edit([&](auto &profile) { id = add_panel(profile); });
    if (!error) panel_id_ = std::move(id);
    changed(error); break;
  }
  case EditorCommand::delete_panel:
    if (fl_choice("Remove this panel? Module references can block this edit.", "Cancel", "Remove", nullptr) == 1)
      changed(session.edit([&](auto &profile) { (void)remove_panel(profile, panel_id_); }));
    break;
  case EditorCommand::delete_binding:
    if (binding_index_ >= 0)
      changed(session.edit([&](auto &profile) {
        (void)remove_binding(profile, static_cast<std::size_t>(binding_index_));
      }));
    break;
  case EditorCommand::add_keyboard:
  case EditorCommand::add_mouse:
  case EditorCommand::capture_binding: {
    const auto index = binding_target->value();
    const auto &items = session.document().profile().cooldowns;
    if (index < 0 || static_cast<std::size_t>(index) >= items.size()) return;
    const auto id = items[static_cast<std::size_t>(index)].id;
    core::InputChord chord;
    if (action == EditorCommand::capture_binding) {
      const auto captured = capture_focused_binding();
      if (!captured) return;
      chord = *captured;
    } else if (action == EditorCommand::add_keyboard) {
      const auto code = parse_count(key_code->value(), 1, std::numeric_limits<std::uint16_t>::max());
      if (!code) { report("Use a physical scan code from 1 to 65535, or focused capture."); return; }
      chord.tokens.push_back({core::InputDevice::keyboard, static_cast<std::uint16_t>(*code), 0});
    } else {
      static constexpr std::uint16_t codes[]{1, 2, 3, 4, 5, 6, 7};
      const auto button = mouse_button->value();
      if (button < 0 || button >= 7) return;
      chord.tokens.push_back({core::InputDevice::mouse, codes[button], 0});
    }
    changed(session.edit([&](auto &profile) { add_binding(profile, id, std::move(chord)); }));
    break;
  }
  default: break;
  }
}
} // namespace layerc::editor
