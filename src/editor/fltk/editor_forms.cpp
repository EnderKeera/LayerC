#include "editor_app.hpp"
#include "accessible_widgets.hpp"
#include "profile_edit.hpp"
#include "profile_mutations.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include "editor_scroll.hpp"
#include <FL/Fl_Tabs.H>

#include <algorithm>
#include <utility>

namespace layerc::editor {

void EditorApp::show() { window.show(); }

bool EditorApp::shown() const {
  return const_cast<EditorWindow &>(window).shown() != 0;
}

bool EditorApp::handle_shortcut(const int key, const int modifiers) {
  return menus && menus->shortcut(key, modifiers);
}

void EditorApp::show_context_menu(const EditorMenuContext context, const bool has_target,
                                  const std::optional<EditorBounds> anchor) {
  if (menus) menus->popup(context, has_target, anchor);
}

void EditorApp::build(std::optional<AppearanceLoad> startup_appearance) {
  window.begin();
  menus = std::make_unique<EditorMenus>(*this, window);
  content = new EditorScroll(0, 0, window.w(), window.h() - EditorMenus::bar_height);
  content->begin();
  FormBuilder builder(*this);
  auto button = [&](int x, int y, int width, const char *label, EditorCommand action) {
    return builder.button(x, y, width, label, action);
  };
  auto browser = [&](int x, int y, int width, int height, const char *label,
                     EditorMenuContext context) {
    return builder.browser(x, y, width, height, label, context);
  };
  // Sidebar (profiles, their commands, preservation and daemon state) beside
  // the tabbed form and preview; one footer row. Everything fits the default
  // window without scrolling; smaller windows scroll instead of overlapping.
  auto *new_profile = button(15, 15, 70, "&New", EditorCommand::new_profile);
  new_profile->shortcut(FL_CTRL | 'n');
  accessibility::describe_with_tooltip(*new_profile, "New profile (Ctrl+N)");
  button(90, 15, 85, "Duplicate", EditorCommand::duplicate);
  accessibility::describe_with_tooltip(*button(180, 15, 75, "Delete", EditorCommand::delete_profile),
                                       "Delete profile");
  profiles = browser(15, 70, 240, 300, "Profiles", EditorMenuContext::profiles);
  button(15, 380, 115, "Toggle saved", EditorCommand::toggle_enabled);
  button(140, 380, 115, "&Import...", EditorCommand::import_profiles);
  button(15, 416, 115, "Export JSON...", EditorCommand::export_json);
  button(140, 416, 115, "Export pack...", EditorCommand::export_pack);
  status_pane_ = std::make_unique<StatusPane>(*this);
  status_pane_->build_modules(builder, 15, 474, 240, 72);

  tabs = new EditorTabs(270, 15, 775, 435);
  tabs->begin();
  profile_page_ = std::make_unique<ProfilePage>(*this);
  profile_page_->build(builder);
  profile_tab = profile_page_->group();

  cooldown_page_ = std::make_unique<CooldownPage>(*this);
  cooldown_page_->build(builder);
  cooldown_tab = cooldown_page_->group();

  binding_page_ = std::make_unique<BindingPage>(*this);
  binding_page_->build(builder);
  binding_tab = binding_page_->group();

  panel_page_ = std::make_unique<PanelPage>(*this);
  panel_page_->build(builder);
  panel_tab = panel_page_->group();
  automation = std::make_unique<LinuxAutomationEditor>(*this);
  visual = std::make_unique<LinuxVisualEditor>(*this);
  tabs->end();
  active_tab_ = profile_tab;
  tabs->callback([](Fl_Widget *, void *data) { static_cast<EditorApp *>(data)->tab_changed(); }, this);
  preview = new PanelPreview(270, 460, 775, 190, *this);
  status_pane_->build_footer(builder);
  content->end();
  content->position(0, EditorMenus::bar_height);
  window.end();
  // Keep the authored form geometry intact, with both-axis scrolling on a
  // smaller screen or at HiDPI rather than clipping or overlapping controls.
  window.size_range(640, 480);
  window.resizable(content);
  window.callback([](Fl_Widget *, void *data) {
    auto &app = *static_cast<EditorApp *>(data);
    app.close_window();
  }, this);
  accessibility = std::make_unique<EditorAccessibility>(*this);
  // The visible line follows every change, including the one that happens
  // while the editor is already open.
  accessibility->set_status_handler([this](const AccessibilityStatus &status) {
    status_pane_->set_accessibility(accessibility_status_line(status));
  });
  load_appearance(std::move(startup_appearance));
  if (!session.profiles().profiles.empty()) {
    if (const auto error = session.open(0)) report(StatusKind::failure, *error);
  }
  refresh();
  const auto initial_kind = session.has_document() ? StatusKind::success
      : session.profiles().profiles.empty() && session.profiles().error.empty()
          ? StatusKind::information : StatusKind::failure;
  report(initial_kind, session.has_document() ? "Profile loaded. Explicit Save applies changes." :
         "No readable profiles. Use New or Import to begin. " + session.profiles().error);
  query_status();
  if (!appearance_error_.empty()) report(StatusKind::warning, appearance_error_);
}

void EditorApp::redraw_preview() {
  if (preview) preview->redraw();
}

void EditorApp::fill_lists() {
  profiles->clear();
  const auto current = session.current_row();
  for (const auto &row : session.profiles().profiles)
    profiles->add(profile_list_text(row.profile.name,
                                    row.profile.game.executable_name,
                                    row.profile.enabled)
                      .c_str());
  profiles->value(current ? static_cast<int>(*current + 1) : 0);
  panel_page_->clear_list();
  cooldown_page_->clear_list();
  binding_page_->clear_list();
  if (!session.has_document()) return;
  const auto &profile = session.document().profile();
  if (!cooldown_index()) cooldown_id_ = profile.cooldowns.empty() ? "" : profile.cooldowns.front().id;
  if (!panel_index()) panel_id_ = profile.panels.empty() ? "" : profile.panels.front().id;
  cooldown_page_->fill_list(profile, cooldown_index());
  panel_page_->fill_list(profile, panel_index());
  if (binding_index_ >= static_cast<int>(profile.bindings.size())) binding_index_ = -1;
  binding_page_->fill_list(profile, binding_index_);
}

void EditorApp::refresh_forms() {
  if (automation) automation->refresh();
  if (visual) visual->refresh();
  if (!session.has_document()) {
    tabs->deactivate();
    profile_page_->show(nullptr);
    status_pane_->set_modules(StatusKind::information, "No open profile.");
    return;
  }
  tabs->activate();
  const auto &profile = session.document().profile();
  profile_page_->show(profile);
  cooldown_page_->show(profile, cooldown_index());
  binding_page_->show(profile);
  panel_page_->show(profile, panel_index());
  StatusKind modules_kind = StatusKind::information;
  std::string modules = "Modules and unsupported bindings are preserved.";
  const auto &document = session.document();
  if (document.has_unreadable_module()) {
    modules_kind = StatusKind::failure;
    modules += "\nUnreadable module: Save is blocked, not repaired by discarding data.";
  }
  if (!document.visual_error().empty()) {
    modules_kind = StatusKind::failure;
    modules += "\nVisual: " + document.visual_error();
  }
  if (!session.profiles().warning_text().empty()) {
    modules_kind = most_severe(modules_kind, StatusKind::warning);
    modules += "\n" + session.profiles().warning_text();
  }
  status_pane_->set_modules(modules_kind, modules);
}
} // namespace layerc::editor
