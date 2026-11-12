#include "editor_commands.hpp"
#include "appearance_window.hpp"
#include "binding_capture.hpp"
#include "cooldown_groups_window.hpp"
#include "diagnostics_window.hpp"
#include "editor_app.hpp"
#include "editor_menus.hpp"
#include "editor_tools.hpp"
#include "profile_edit.hpp"
#include "profile_mutations.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace layerc::editor {
namespace {

using Context = EditorMenuContext;
constexpr unsigned profiles = context_bit(Context::profiles);
constexpr unsigned cooldowns = context_bit(Context::cooldowns);
constexpr unsigned bindings = context_bit(Context::bindings);
constexpr unsigned panels = context_bit(Context::panels);

constexpr std::size_t row_count =
    static_cast<std::size_t>(EditorCommand::count);

} // namespace

// The single command table. Each row's lambdas are captureless so they decay to
// the plain function pointers the spec stores; being members of this friend
// struct lets them use EditorApp's private state.
struct EditorCommandRegistry {
  static const std::array<EditorCommandSpec, row_count> &rows();
};

const std::array<EditorCommandSpec, row_count> &EditorCommandRegistry::rows() {
  static const std::array<EditorCommandSpec, row_count> table{{
      {EditorCommand::new_profile, "&File/&New profile", "&New profile",
       FL_CTRL | 'n', 0, 0, profiles, 0,
       +[](const EditorApp &) { return true; },
       +[](EditorApp &app) {
         if (!app.may_leave()) return;
         app.session.new_profile();
         app.cooldown_id_.clear();
         app.panel_id_.clear();
         app.active_tab_ = app.profile_tab;
         app.tabs->value(app.profile_tab);
         app.refresh();
         app.report("New unsaved profile. Enter an executable name or path.");
       }},
      {EditorCommand::import_profiles, "&File/&Import...", "&Import...",
       FL_CTRL | 'o', 0, 0, profiles, 0,
       +[](const EditorApp &) { return true; },
       +[](EditorApp &app) { app.import_profiles(); }},
      {EditorCommand::export_json, "&File/Export &JSON...", "Export &JSON...",
       FL_CTRL | FL_SHIFT | 'e', 0, 0, profiles, 0,
       +[](const EditorApp &app) { return app.session.has_document(); },
       +[](EditorApp &app) {
         if (!app.session.has_document()) {
           app.report("Create or select a profile first.");
           return;
         }
         app.export_profile(false);
       }},
      {EditorCommand::export_pack, "&File/Export &pack...", "Export &pack...",
       FL_CTRL | FL_ALT | FL_SHIFT | 'e', 0, FL_MENU_DIVIDER, profiles, 0,
       +[](const EditorApp &app) { return app.session.has_document(); },
       +[](EditorApp &app) {
         if (!app.session.has_document()) {
           app.report("Create or select a profile first.");
           return;
         }
         app.export_profile(true);
       }},
      {EditorCommand::save, "&File/&Save", "&Save", FL_CTRL | 's', 0, 0,
       profiles, 0,
       +[](const EditorApp &app) { return app.session.has_document(); },
       +[](EditorApp &app) { (void)app.save(); }},
      {EditorCommand::save_as, "&File/Save &as...", "Save &as...",
       FL_CTRL | FL_SHIFT | 's', 0, 0, 0, 0,
       +[](const EditorApp &) { return true; },
       +[](EditorApp &app) {
         if (!app.session.has_document()) {
           app.report("Open a profile before Save As.");
           return;
         }
         if (!app.apply_fields()) {
           app.report("Save As needs valid fields first.");
           return;
         }
         const auto current = app.session.document().profile().name;
         fl_message_title("Save profile as");
         const char *chosen_name = fl_input(
             "Save the open profile, with its modules, as a new profile named:",
             (current + " (copy)").c_str());
         if (!chosen_name) return;
         if (std::string_view(chosen_name).empty()) {
           app.report("Save As needs a profile name.");
           return;
         }
         const std::string chosen(chosen_name);
         if (const auto error = app.session.duplicate_current()) {
           app.report(*error);
           app.refresh();
           return;
         }
         if (const auto error =
                 app.session.edit([&](auto &profile) { profile.name = chosen; })) {
           app.report(*error);
           app.refresh();
           return;
         }
         app.refresh();
         (void)app.save();
       }},
      {EditorCommand::open_folder, "&File/Open profiles &folder",
       "Open profiles &folder", 0, 0, FL_MENU_DIVIDER, 0, 0,
       +[](const EditorApp &) { return true; },
       +[](EditorApp &app) { app.open_profiles_folder(); }},
      {EditorCommand::close, "&File/&Quit", "&Quit", FL_CTRL | 'q', 0, 0, 0, 0,
       +[](const EditorApp &) { return true; },
       +[](EditorApp &app) {
         if (app.may_leave()) {
           app.stop_edit_services();
           app.window.hide();
         }
       }},
      {EditorCommand::undo, "&Edit/&Undo", "&Undo", FL_CTRL | 'z', 0, 0, 0, 0,
       +[](const EditorApp &app) {
         return app.session.has_document() &&
                (app.pending_fields_ || app.session.can_undo());
       },
       +[](EditorApp &app) {
         if (!app.session.has_document()) {
           app.report("Create or select a profile first.");
           return;
         }
         if (app.pending_fields_) {
           app.refresh();
           app.report("Invalid field edit discarded.");
           return;
         }
         app.session.end_text_edit();
         const bool done = app.session.undo();
         app.refresh();
         app.report(done ? "History restored. " + app.edit_status()
                         : "No history in that direction.");
       }},
      {EditorCommand::redo, "&Edit/&Redo", "&Redo", FL_CTRL | FL_SHIFT | 'z',
       FL_CTRL | 'y', FL_MENU_DIVIDER, 0, 0,
       +[](const EditorApp &app) {
         return app.session.has_document() && !app.pending_fields_ &&
                app.session.can_redo();
       },
       +[](EditorApp &app) {
         if (!app.session.has_document()) {
           app.report("Create or select a profile first.");
           return;
         }
         if (app.pending_fields_) {
           app.refresh();
           app.report("Invalid field edit discarded.");
           return;
         }
         app.session.end_text_edit();
         const bool done = app.session.redo();
         app.refresh();
         app.report(done ? "History restored. " + app.edit_status()
                         : "No history in that direction.");
       }},
      {EditorCommand::rename, "&Edit/Re&name", "Re&name", FL_F + 2, 0, 0, 0, 0,
       +[](const EditorApp &app) { return app.session.has_document(); },
       +[](EditorApp &app) {
         const bool cooldown =
             app.active_tab_ == app.cooldown_tab && app.cooldown_index();
         if (!cooldown) {
           app.tabs->value(app.profile_tab);
           app.tab_changed();
           app.tabs->redraw();
         }
         if (app.active_tab_ != (cooldown ? app.cooldown_tab : app.profile_tab))
           return; // invalid field kept
         if (cooldown)
           app.cooldown_page_->focus_name();
         else
           app.profile_page_->focus_name();
       }},
      {EditorCommand::duplicate, "&Profile/&Duplicate", "&Duplicate",
       FL_CTRL | 'd', 0, 0, profiles, profiles,
       +[](const EditorApp &app) {
         return app.session.has_document() && app.session.current_row().has_value();
       },
       +[](EditorApp &app) {
         if (!app.may_leave()) return;
         app.changed(app.session.duplicate_current());
       }},
      {EditorCommand::delete_profile, "&Profile/&Delete...", "&Delete...", 0, 0,
       0, profiles, profiles,
       +[](const EditorApp &app) {
         return app.session.has_document() && app.session.current_row().has_value();
       },
       +[](EditorApp &app) {
         if (!app.may_leave()) return;
         if (!app.session.has_document()) {
           app.report("Draft discarded.");
           return;
         }
         fl_message_title("Delete profile");
         if (fl_choice("Delete this profile, all modules and recovery copies?\n"
                       "This cannot be undone.",
                       "Cancel", "Delete", nullptr) != 1)
           return;
         const auto outcome = app.session.delete_current();
         app.refresh();
         app.report(outcome.text);
         app.query_status();
       }},
      {EditorCommand::toggle_enabled, "&Profile/Toggle saved &enabled",
       "Toggle saved &enabled", 0, 0, 0, profiles, profiles,
       +[](const EditorApp &app) {
         return app.session.has_document() && app.session.current_row().has_value();
       },
       +[](EditorApp &app) {
         if (!app.may_leave()) return;
         const auto row = app.session.current_row();
         if (!row) {
           app.report("Save this draft before toggling it on disk.");
           return;
         }
         const auto outcome = app.session.set_enabled(
             *row, !app.session.document().profile().enabled,
             confirm_profile_upgrade);
         app.refresh();
         app.report(outcome.text);
         app.query_status();
       }},
      {EditorCommand::add_cooldown, "&Cooldowns/&Add cooldown", "&Add cooldown", 0,
       0, 0, cooldowns, 0,
       +[](const EditorApp &app) { return app.session.has_document(); },
       +[](EditorApp &app) {
         if (!app.session.has_document()) {
           app.report("Create or select a profile first.");
           return;
         }
         if (!app.apply_fields()) return;
         app.session.end_text_edit();
         std::string id;
         const auto error =
             app.session.edit([&](auto &profile) { id = add_cooldown(profile); });
         if (!error) app.cooldown_id_ = std::move(id);
         app.changed(error);
       }},
      {EditorCommand::duplicate_cooldown, "&Cooldowns/D&uplicate cooldown",
       "D&uplicate cooldown", 0, 0, 0, cooldowns, cooldowns,
       +[](const EditorApp &app) { return app.cooldown_index().has_value(); },
       +[](EditorApp &app) {
         if (!app.session.has_document()) {
           app.report("Create or select a profile first.");
           return;
         }
         if (!app.apply_fields()) return;
         app.session.end_text_edit();
         std::string id;
         const auto error = app.session.edit(
             [&](auto &profile) { id = duplicate_cooldown(profile, app.cooldown_id_); });
         if (!error && id.empty()) {
           app.report("Select a cooldown to duplicate.");
           return;
         }
         if (!error) app.cooldown_id_ = std::move(id);
         app.changed(error);
         if (!error)
           app.report(
               "Cooldown duplicated without bindings; add a binding for the copy.");
       }},
      {EditorCommand::delete_cooldown, "&Cooldowns/&Remove cooldown...",
       "&Remove cooldown...", 0, 0, 0, cooldowns, cooldowns,
       +[](const EditorApp &app) { return app.cooldown_index().has_value(); },
       +[](EditorApp &app) {
         if (!app.session.has_document()) {
           app.report("Create or select a profile first.");
           return;
         }
         if (!app.apply_fields()) return;
         app.session.end_text_edit();
         fl_message_title("Remove cooldown");
         if (fl_choice("Remove this cooldown, its bindings and memberships?",
                       "Cancel", "Remove", nullptr) == 1)
           app.changed(app.session.remove_cooldown(app.cooldown_id_));
       }},
      {EditorCommand::cooldown_groups, "&Cooldowns/Edit &groups...",
       "Edit &groups...", 0, 0, 0, cooldowns, 0,
       +[](const EditorApp &app) { return app.session.has_document(); },
       +[](EditorApp &app) {
         if (!app.session.has_document() || !app.apply_fields()) return;
         CooldownGroupsDialog dialog(app.session);
         apply_editor_appearance(app.appearance_, dialog, nullptr, nullptr);
         dialog.show();
         while (dialog.shown()) Fl::wait();
         if (dialog.applied)
           app.changed(std::nullopt);
         else
           app.report("Cooldown groups unchanged.");
       }},
      {EditorCommand::add_keyboard, "&Bindings/Add &key", "Add &key", 0, 0, 0,
       bindings, 0,
       +[](const EditorApp &app) {
         return app.session.has_document() &&
                !app.session.document().profile().cooldowns.empty();
       },
       +[](EditorApp &app) {
         const auto index = app.binding_page_->target();
         const auto &items = app.session.document().profile().cooldowns;
         if (index < 0 || static_cast<std::size_t>(index) >= items.size()) return;
         const auto id = items[static_cast<std::size_t>(index)].id;
         core::InputChord chord;
         const auto code = parse_count(app.binding_page_->key_code(), 1,
                                       std::numeric_limits<std::uint16_t>::max());
         if (!code) {
           app.report("Use a physical scan code from 1 to 65535, or focused capture.");
           return;
         }
         chord.tokens.push_back(
             core::InputToken{core::InputDevice::keyboard,
                              static_cast<std::uint16_t>(*code), 0});
         app.changed(app.session.edit(
             [&](auto &profile) { add_binding(profile, id, std::move(chord)); }));
       }},
      {EditorCommand::add_mouse, "&Bindings/Add &mouse", "Add &mouse", 0, 0, 0,
       bindings, 0,
       +[](const EditorApp &app) {
         return app.session.has_document() &&
                !app.session.document().profile().cooldowns.empty();
       },
       +[](EditorApp &app) {
         const auto index = app.binding_page_->target();
         const auto &items = app.session.document().profile().cooldowns;
         if (index < 0 || static_cast<std::size_t>(index) >= items.size()) return;
         const auto id = items[static_cast<std::size_t>(index)].id;
         core::InputChord chord;
         static constexpr std::uint16_t codes[]{1, 2, 3, 4, 5, 6, 7};
         const auto button = app.binding_page_->mouse();
         if (button < 0 || button >= 7) return;
         chord.tokens.push_back(
             core::InputToken{core::InputDevice::mouse, codes[button], 0});
         app.changed(app.session.edit(
             [&](auto &profile) { add_binding(profile, id, std::move(chord)); }));
       }},
      {EditorCommand::capture_binding, "&Bindings/&Capture focused chord...",
       "&Capture focused chord...", 0, 0, 0, bindings, 0,
       +[](const EditorApp &app) {
         return app.session.has_document() &&
                !app.session.document().profile().cooldowns.empty();
       },
       +[](EditorApp &app) {
         const auto index = app.binding_page_->target();
         const auto &items = app.session.document().profile().cooldowns;
         if (index < 0 || static_cast<std::size_t>(index) >= items.size()) return;
         const auto id = items[static_cast<std::size_t>(index)].id;
         const auto captured = capture_focused_binding();
         if (!captured) return;
         app.changed(app.session.edit([&](auto &profile) {
           add_binding(profile, id, *captured);
         }));
       }},
      {EditorCommand::delete_binding, "&Bindings/&Remove binding",
       "&Remove binding", 0, 0, 0, bindings, bindings,
       +[](const EditorApp &app) {
         return app.session.has_document() && app.binding_index_ >= 0 &&
                static_cast<std::size_t>(app.binding_index_) <
                    app.session.document().profile().bindings.size();
       },
       +[](EditorApp &app) {
         if (!app.session.has_document()) {
           app.report("Create or select a profile first.");
           return;
         }
         if (!app.apply_fields()) return;
         app.session.end_text_edit();
         if (app.binding_index_ >= 0)
           app.changed(app.session.edit([&](auto &profile) {
             (void)remove_binding(profile, static_cast<std::size_t>(app.binding_index_));
           }));
       }},
      {EditorCommand::add_panel, "&Panels/&Add panel", "&Add panel", 0, 0, 0,
       panels, 0,
       +[](const EditorApp &app) { return app.session.has_document(); },
       +[](EditorApp &app) {
         if (!app.session.has_document()) {
           app.report("Create or select a profile first.");
           return;
         }
         if (!app.apply_fields()) return;
         app.session.end_text_edit();
         std::string id;
         const auto error =
             app.session.edit([&](auto &profile) { id = add_panel(profile); });
         if (!error) app.panel_id_ = std::move(id);
         app.changed(error);
       }},
      {EditorCommand::delete_panel, "&Panels/&Remove panel...",
       "&Remove panel...", 0, 0, 0, panels, panels,
       +[](const EditorApp &app) { return app.panel_index().has_value(); },
       +[](EditorApp &app) {
         if (!app.session.has_document()) {
           app.report("Create or select a profile first.");
           return;
         }
         if (!app.apply_fields()) return;
         app.session.end_text_edit();
         fl_message_title("Remove panel");
         if (fl_choice("Remove this panel? Module references can block this edit.",
                       "Cancel", "Remove", nullptr) == 1)
           app.changed(app.session.edit(
               [&](auto &profile) { (void)remove_panel(profile, app.panel_id_); }));
       }},
      {EditorCommand::panel_colors, "&Panels/Choose &colors...",
       "Choose &colors...", 0, 0, 0, panels, panels,
       +[](const EditorApp &app) { return app.panel_index().has_value(); },
       +[](EditorApp &app) {
         const auto index = app.panel_index();
         if (!index || !app.apply_fields()) return;
         const auto &bar = app.session.document().profile().panels[*index].bar;
         const auto chosen = edit_panel_colors(
             PanelColorValues{{bar.fill_color, bar.background_color,
                               bar.border_color, bar.text_color}});
         if (!chosen) {
           app.report("Panel colors unchanged.");
           return;
         }
         const auto id = app.panel_id_;
         app.changed(app.session.edit([&](auto &profile) {
           for (auto &panel : profile.panels) {
             if (panel.id != id) continue;
             panel.bar.fill_color = chosen->values[0];
             panel.bar.background_color = chosen->values[1];
             panel.bar.border_color = chosen->values[2];
             panel.bar.text_color = chosen->values[3];
           }
         }));
       }},
      {EditorCommand::position, "&Panels/&Position on window",
       "&Position on window", 0, 0, 0, 0, 0,
       +[](const EditorApp &app) { return app.session.has_document(); },
       +[](EditorApp &app) {
         if (!app.session.has_document()) {
           app.report("Create or select a profile first.");
           return;
         }
         app.position_command();
       }},
      {EditorCommand::profile_page, "&View/&Profile", "&Profile", FL_CTRL | '1',
       0, 0, 0, 0, +[](const EditorApp &) { return true; },
       +[](EditorApp &app) {
         app.tabs->value(app.profile_tab);
         app.tab_changed();
         app.tabs->redraw();
       }},
      {EditorCommand::cooldown_page, "&View/&Cooldowns", "&Cooldowns",
       FL_CTRL | '2', 0, 0, 0, 0, +[](const EditorApp &) { return true; },
       +[](EditorApp &app) {
         app.tabs->value(app.cooldown_tab);
         app.tab_changed();
         app.tabs->redraw();
       }},
      {EditorCommand::binding_page, "&View/&Bindings", "&Bindings",
       FL_CTRL | '3', 0, 0, 0, 0, +[](const EditorApp &) { return true; },
       +[](EditorApp &app) {
         app.tabs->value(app.binding_tab);
         app.tab_changed();
         app.tabs->redraw();
       }},
      {EditorCommand::panel_page, "&View/&Panels", "&Panels", FL_CTRL | '4', 0,
       FL_MENU_DIVIDER, 0, 0, +[](const EditorApp &) { return true; },
       +[](EditorApp &app) {
         app.tabs->value(app.panel_tab);
         app.tab_changed();
         app.tabs->redraw();
       }},
      {EditorCommand::automation_page, "&View/&Automation", "&Automation",
       FL_CTRL | '5', 0, 0, 0, 0, +[](const EditorApp &) { return true; },
       +[](EditorApp &app) {
         if (app.automation) app.tabs->value(app.automation->group());
         app.tab_changed();
         app.tabs->redraw();
       }},
      {EditorCommand::visual_page, "&View/&Visual", "&Visual", FL_CTRL | '6', 0,
       0, 0, 0, +[](const EditorApp &) { return true; },
       +[](EditorApp &app) {
         if (app.visual) app.tabs->value(app.visual->group());
         app.tab_changed();
         app.tabs->redraw();
       }},
      {EditorCommand::refresh, "&View/LayerD &status", "LayerD &status", 0, 0, 0,
       0, 0, +[](const EditorApp &) { return true; },
       +[](EditorApp &app) { app.query_status(); }},
      {EditorCommand::diagnostics, "&View/Runtime &diagnostics...",
       "Runtime &diagnostics...", 0, 0, 0, 0, 0,
       +[](const EditorApp &) { return true; },
       +[](EditorApp &app) {
         if (!app.diagnostics) app.diagnostics = std::make_unique<DiagnosticsWindow>();
         app.diagnostics->show();
       }},
      {EditorCommand::options, "&Tools/&Options...", "&Options...", FL_CTRL | ',',
       0, 0, 0, 0, +[](const EditorApp &) { return true; },
       +[](EditorApp &) { show_text_dialog("Options — LayerE", options_text()); }},
      {EditorCommand::appearance, "&Tools/A&ppearance...", "A&ppearance...", 0, 0,
       FL_MENU_DIVIDER, 0, 0, +[](const EditorApp &) { return true; },
       +[](EditorApp &app) {
         if (!app.appearance_error_.empty()) {
           fl_message_title("Editor appearance");
           if (fl_choice("%s\nApplying a new choice replaces the file.", "Cancel",
                         "Continue", nullptr, app.appearance_error_.c_str()) != 1)
             return;
         }
         const auto chosen =
             edit_editor_appearance(app.appearance_, app.window, app.profiles, app.preview);
         if (!chosen) {
           app.report("Appearance unchanged.");
           return;
         }
         app.appearance_ = *chosen;
         if (const auto error =
                 save_editor_appearance(app.appearance_path_, app.appearance_)) {
           app.report(*error + " The choice applies until LayerE closes.");
         } else {
           app.appearance_error_.clear();
           app.report("Appearance saved: " +
                      std::string(theme_label(app.appearance_.theme)) + ".");
         }
       }},
      {EditorCommand::split_bars, "&Tools/Split grouped &bars",
       "Split grouped &bars", 0, 0, FL_MENU_DIVIDER, 0, 0,
       +[](const EditorApp &) { return true; },
       +[](EditorApp &app) {
         if (!app.apply_fields()) return;
         std::optional<std::size_t> first;
         const auto error = app.session.edit([&](auto &profile) {
           first = split_grouped_bars(profile, app.preview->h());
         });
         if (error) {
           app.changed(error);
           return;
         }
         if (!first) {
           app.report("Bars are already independent; nothing was split.");
           return;
         }
         app.panel_id_ = app.session.document().profile().panels[*first].id;
         app.changed(std::nullopt);
         app.report("Split grouped bars into independently positioned cooldown "
                    "bars. " +
                    app.edit_status());
       }},
      {EditorCommand::tasks, "&Tools/Detected &tasks...", "Detected &tasks...",
       FL_CTRL | 't', 0, 0, 0, 0, +[](const EditorApp &) { return true; },
       +[](EditorApp &app) { app.show_tasks(); }},
      {EditorCommand::shortcuts_help, "&Help/&Keyboard shortcuts",
       "&Keyboard shortcuts", FL_F + 1, 0, 0, 0, 0,
       +[](const EditorApp &) { return true; },
       +[](EditorApp &) {
         show_shortcuts_dialog(shortcut_help());
       }},
      {EditorCommand::about, "&Help/&About LayerE", "&About LayerE", 0, 0, 0, 0,
       0, +[](const EditorApp &) { return true; },
       +[](EditorApp &app) {
         show_about_dialog(app.daemon_text_,
                           app.status_pane_->accessibility_text());
       }},
  }};
  return table;
}

std::span<const EditorCommandSpec> editor_command_specs() {
  return EditorCommandRegistry::rows();
}

const EditorCommandSpec &command_spec(const EditorCommand command) {
  const auto &rows = EditorCommandRegistry::rows();
  const auto index = static_cast<std::size_t>(command);
  if (index < rows.size()) return rows[index];
  return rows.front();
}

bool command_enabled(const EditorApp &app, const EditorCommand command) {
  const auto &spec = command_spec(command);
  return spec.enabled == nullptr || spec.enabled(app);
}

void run_command(EditorApp &app, const EditorCommand command) {
  const auto &spec = command_spec(command);
  if (!command_enabled(app, command) || spec.run == nullptr) return;
  spec.run(app);
}

std::string shortcut_help() {
  std::string text;
  const auto describe = [&](const EditorCommandSpec &spec, const int key) {
    std::string label = spec.label == nullptr ? std::string{} : spec.label;
    std::erase(label, '&');
    text += std::string(fl_shortcut_label(static_cast<unsigned>(key))) + "\t" + label +
            "\n";
  };
  for (const auto &spec : EditorCommandRegistry::rows())
    if (spec.shortcut != 0) describe(spec, spec.shortcut);
  for (const auto &spec : EditorCommandRegistry::rows())
    if (spec.alternate_shortcut != 0) describe(spec, spec.alternate_shortcut);
  return text + "Tab / Shift+Tab\tMove between fields\n"
                "Alt+letter\tOpen the menu with that underlined letter\n"
                "Right-click\tList context menu\n"
                "Arrow keys\tChange the focused list, choice or tab";
}

} // namespace layerc::editor
