#pragma once

// Tests-only access to the editor's widgets: the pages own them, so tests reach
// them through this friend struct instead of public Fl_* members.
#include "editor_app.hpp"
#include "editor_style.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Tabs.H>

#include <cstdint>
#include <memory>

namespace layerc::editor {

struct EditorAppTestAccess {
  static Fl_Timeout_Handler steady_timeout_handler() { return &SteadyTimeout::handle; }

  // Autosave timing seam.
  static EditorApp with_timer_scheduler(SteadyTimeout::Now now,
                                        SteadyTimeout::Scheduler scheduler) {
    return EditorApp(now, scheduler);
  }
  static bool autosave_pending(const EditorApp &app) {
    return app.autosave_timer_.pending();
  }
  static std::uint64_t live_sequence(const EditorApp &app) {
    return app.live_draft_sequence_;
  }
  // The status column and footer.
  static Fl_Multiline_Output *status(EditorApp &app) {
    return app.status_pane_ ? app.status_pane_->status() : nullptr;
  }
  static Fl_Multiline_Output *daemon_status(EditorApp &app) {
    return app.status_pane_ ? app.status_pane_->daemon() : nullptr;
  }
  static Fl_Multiline_Output *module_status(EditorApp &app) {
    return app.status_pane_ ? app.status_pane_->modules() : nullptr;
  }
  static Fl_Box *accessibility_status(EditorApp &app) {
    return app.status_pane_ ? app.status_pane_->accessibility() : nullptr;
  }
  static Fl_Button *undo_button(EditorApp &app) {
    return app.status_pane_ ? app.status_pane_->undo() : nullptr;
  }
  static Fl_Button *redo_button(EditorApp &app) {
    return app.status_pane_ ? app.status_pane_->redo() : nullptr;
  }
  static StatusKind status_kind(EditorApp &app) {
    return app.status_pane_ && app.status_pane_->status_mark_
        ? app.status_pane_->status_mark_->kind() : StatusKind::none;
  }
  // The Profile tab.
  static Fl_Group *profile_tab(EditorApp &app) {
    return app.profile_page_ ? app.profile_page_->group() : nullptr;
  }
  static Fl_Input *name(EditorApp &app) {
    return app.profile_page_ ? app.profile_page_->name() : nullptr;
  }
  static Fl_Input *executable(EditorApp &app) {
    return app.profile_page_ ? app.profile_page_->executable() : nullptr;
  }
  static Fl_Input *executable_path(EditorApp &app) {
    return app.profile_page_ ? app.profile_page_->executable_path() : nullptr;
  }
  static Fl_Input *window_class(EditorApp &app) {
    return app.profile_page_ ? app.profile_page_->window_class() : nullptr;
  }
  static Fl_Input *window_title(EditorApp &app) {
    return app.profile_page_ ? app.profile_page_->window_title() : nullptr;
  }
  static Fl_Check_Button *enabled(EditorApp &app) {
    return app.profile_page_ ? app.profile_page_->enabled() : nullptr;
  }
  static Fl_Input *profile_id(EditorApp &app) {
    return app.profile_page_ ? app.profile_page_->profile_id() : nullptr;
  }
  static Fl_Button *browse_application(EditorApp &app) {
    return app.profile_page_->browse_;
  }
  static void set_application_chooser(EditorApp &app,
                                      std::function<std::optional<std::string>()> choose) {
    app.profile_page_->choose_application_ = std::move(choose);
  }

  // The Cooldowns tab.
  static Fl_Button *remove_cooldown(EditorApp &app) { return app.cooldown_page_->remove_; }
  static Fl_Box *remove_cooldown_reason(EditorApp &app) { return app.cooldown_page_->remove_reason_; }
  static ContextBrowser *cooldowns(EditorApp &app) {
    return app.cooldown_page_ ? app.cooldown_page_->list() : nullptr;
  }
  static Fl_Input *cooldown_name(EditorApp &app) {
    return app.cooldown_page_ ? app.cooldown_page_->name() : nullptr;
  }
  static Fl_Input *duration(EditorApp &app) {
    return app.cooldown_page_ ? app.cooldown_page_->duration() : nullptr;
  }
  static Fl_Input *active_duration(EditorApp &app) {
    return app.cooldown_page_ ? app.cooldown_page_->active_duration() : nullptr;
  }
  static Fl_Input *charges(EditorApp &app) {
    return app.cooldown_page_ ? app.cooldown_page_->charges() : nullptr;
  }
  static Fl_Choice *recovery(EditorApp &app) {
    return app.cooldown_page_ ? app.cooldown_page_->recovery() : nullptr;
  }
  static Fl_Choice *retrigger(EditorApp &app) {
    return app.cooldown_page_ ? app.cooldown_page_->retrigger() : nullptr;
  }
  // The Bindings tab.
  static ContextBrowser *bindings(EditorApp &app) {
    return app.binding_page_ ? app.binding_page_->list() : nullptr;
  }
  static Fl_Choice *binding_target(EditorApp &app) {
    return app.binding_page_ ? app.binding_page_->target_choice() : nullptr;
  }
  static Fl_Input *key_code(EditorApp &app) {
    return app.binding_page_ ? app.binding_page_->key_code_field() : nullptr;
  }
  static Fl_Choice *mouse_button(EditorApp &app) {
    return app.binding_page_ ? app.binding_page_->mouse_choice() : nullptr;
  }


  // The Panels tab.
  static ContextBrowser *panels(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->panels_ : nullptr;
  }
  static Fl_Group *panel_fields(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->panel_fields_ : nullptr;
  }
  static Fl_Input *panel_name(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->panel_name_ : nullptr;
  }
  static Fl_Input *anchor_x(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->anchor_x_ : nullptr;
  }
  static Fl_Input *anchor_y(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->anchor_y_ : nullptr;
  }
  static Fl_Input *bar_width(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->bar_width_ : nullptr;
  }
  static Fl_Input *bar_height(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->bar_height_ : nullptr;
  }
  static Fl_Input *fill(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->fill_ : nullptr;
  }
  static Fl_Input *background(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->background_ : nullptr;
  }
  static Fl_Input *border(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->border_ : nullptr;
  }
  static Fl_Input *text_color(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->text_color_ : nullptr;
  }
  static Fl_Choice *layout(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->layout_ : nullptr;
  }
  static Fl_Choice *style(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->style_ : nullptr;
  }
  static Fl_Hold_Browser *items(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->items_.items_ : nullptr;
  }
  static Fl_Button *add_item(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->items_.add_ : nullptr;
  }
  static Fl_Button *edit_item(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->items_.edit_ : nullptr;
  }
  static Fl_Button *remove_item(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->items_.remove_ : nullptr;
  }
  static Fl_Box *item_reason(EditorApp &app) { return app.panel_page_->items_.reason_; }
  static Fl_Button *item_up(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->items_.up_ : nullptr;
  }
  static Fl_Button *item_down(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->items_.down_ : nullptr;
  }
  static Fl_Check_Button *show_text(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->show_text_ : nullptr;
  }
  static Fl_Check_Button *hide_ready(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->hide_ready_ : nullptr;
  }
  static Fl_Button *position_button(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->position_button_ : nullptr;
  }
  static void set_on_window_edit(EditorApp &app, std::unique_ptr<OnWindowEdit> on_window) {
    app.on_window_ = std::move(on_window);
  }
  static void position_on(EditorApp &app, const TaskCandidate &task) { app.position_on(task); }
  static void on_window_ready(EditorApp &app) { app.on_window_ready(); }
  static void acknowledge_direct_drag_notice(EditorApp &app) { app.direct_drag_notice_shown_ = true; }
  static void set_direct_drag_confirmation(EditorApp &app, std::function<int()> confirm) {
    app.confirm_direct_drag_for_test_ = std::move(confirm);
  }
  static Fl_Check_Button *direct_drag(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->direct_drag_ : nullptr;
  }
  static Fl_Box *contrast_line(EditorApp &app) {
    return app.panel_page_ ? app.panel_page_->contrast_line_ : nullptr;
  }

  static EditorWindow &window(EditorApp &app) { return app.window; }
  static Fl_Scroll *content(EditorApp &app) { return app.content; }
  static Fl_Hold_Browser *profiles(EditorApp &app) { return app.profiles; }
  static Fl_Tabs *tabs(EditorApp &app) { return app.tabs; }
  static Fl_Group *cooldown_tab(EditorApp &app) { return app.cooldown_tab; }
  static Fl_Group *binding_tab(EditorApp &app) { return app.binding_tab; }
  static Fl_Group *panel_tab(EditorApp &app) { return app.panel_tab; }
  static PanelPreview *preview(EditorApp &app) { return app.preview; }
  static SimulatorPanelPreview preview_panel(EditorApp &app, const core::Panel &panel) {
    return app.preview->present(panel);
  }
  static EditorMenus *menus(EditorApp &app) { return app.menus.get(); }
  static LinuxAutomationEditor *automation(EditorApp &app) { return app.automation.get(); }
  static LinuxVisualEditor *visual(EditorApp &app) { return app.visual.get(); }
  static DiagnosticsWindow *diagnostics(EditorApp &app) { return app.diagnostics.get(); }
  static TasksWindow *tasks_window(EditorApp &app) { return app.tasks_window.get(); }
  static EditorAccessibility *accessibility(EditorApp &app) { return app.accessibility.get(); }
  // Drops the app's bridge so a test can own the single AT-SPI adaptor itself;
  // the app only consults its accessibility object during build().
  static void release_accessibility(EditorApp &app) { app.accessibility.reset(); }
};

} // namespace layerc::editor
