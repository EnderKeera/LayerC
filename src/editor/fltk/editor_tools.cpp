// Tools and file commands beyond the core edit loop: Save As, the profiles
// folder, Split grouped bars, panel colours, Options and editor Appearance.
#include "editor_tools.hpp"
#include "accessible_widgets.hpp"
#include "editor_app.hpp"
#include "platform/editor_appearance_store.hpp"
#include "platform/product_text.hpp"
#include "appearance_window.hpp"
#include "cooldown_groups_window.hpp"
#include "diagnostics_window.hpp"
#include "profile_edit.hpp"
#include "profile_mutations.hpp"

#include "editor_style.hpp"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <array>
#include <utility>

namespace layerc::editor {
namespace {

struct PanelColorSet {
  std::array<std::uint32_t, 4> values{};
};
constexpr std::array<const char *, 4> color_names{"Fill", "Background", "Border", "Text"};
constexpr int input_width = 140;
constexpr int button_width = 155;

// Modal panel colours: a #RRGGBB field and a chooser per bar colour.
std::optional<PanelColorSet> edit_panel_colors_internal(const PanelColorSet &current) {
  const auto layout = panel_colors_layout();
  Fl_Double_Window dialog(layout.window_width, 250, "Panel colors — LayerE");
  std::array<Fl_Input *, 4> fields{};
  std::array<Fl_Button *, 4> buttons{};
  struct Pick {
    Fl_Input *field;
    Fl_Button *button;
    const char *title;
    void sync_name() const {
      const auto color = parse_rgb(field->value()).value_or(current_editor_tokens().disabled);
      accessibility::set_accessible_name(
          *button, accessibility::color_button_name(title, format_rgb(color)));
    }
  };
  std::array<Pick, 4> picks{};
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const int y = 15 + static_cast<int>(index) * 40;
    fields[index] = new EditorField<Fl_Input>(layout.input_x, y, input_width, editor_input_height, nullptr);
    fields[index]->copy_label((std::string(color_names[index]) + " #RRGGBB").c_str());
    fields[index]->value(format_rgb(current.values[index]).c_str());
    auto *choose = new Fl_Button(layout.button_x, y, button_width, editor_button_height, nullptr);
    buttons[index] = choose;
    choose->copy_label(("Choose " + std::string(color_names[index]) + "...").c_str());
    picks[index] = {fields[index], choose, color_names[index]};
    picks[index].sync_name();
    fields[index]->when(FL_WHEN_CHANGED);
    fields[index]->callback([](Fl_Widget *, void *data) {
      static_cast<const Pick *>(data)->sync_name();
    }, &picks[index]);
    choose->callback([](Fl_Widget *, void *data) {
      auto &pick = *static_cast<Pick *>(data);
      const auto initial = parse_rgb(pick.field->value()).value_or(current_editor_tokens().disabled);
      if (const auto color = choose_color(pick.title, initial)) {
        pick.field->value(format_rgb(*color).c_str());
        pick.sync_name();
      }
    }, &picks[index]);
  }
  auto *message = new Fl_Box(15, 170, layout.window_width - 30, 28);
  message->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  for (std::size_t index = 0; index < fields.size(); ++index) {
    accessibility::describe_with(*fields[index], *message);
    accessibility::describe_with(*buttons[index], *message);
  }
  bool accepted = false;
  auto *cancel = new Fl_Button(layout.window_width - 250, 205, 110, editor_button_height, "Cancel");
  cancel->callback([](Fl_Widget *widget, void *) { widget->window()->hide(); });
  struct Accept { bool *accepted; std::array<Fl_Input *, 4> *fields; Fl_Box *message; };
  Accept accept_state{&accepted, &fields, message};
  auto *ok = new Fl_Button(layout.window_width - 125, 205, 110, editor_button_height, "OK");
  ok->callback([](Fl_Widget *widget, void *data) {
    auto &state = *static_cast<Accept *>(data);
    for (std::size_t index = 0; index < state.fields->size(); ++index)
      if (!parse_rgb((*state.fields)[index]->value())) {
        state.message->copy_label((std::string(color_names[index]) +
                                   " must be #RRGGBB; nothing was changed.").c_str());
        state.message->redraw();
        return;
      }
    *state.accepted = true;
    widget->window()->hide();
  }, &accept_state);
  dialog.end();
  dialog.set_modal();
  dialog.show();
  while (dialog.shown()) Fl::wait();
  if (!accepted) return std::nullopt;
  PanelColorSet result;
  for (std::size_t index = 0; index < fields.size(); ++index)
    result.values[index] = *parse_rgb(fields[index]->value());
  return result;
}

} // namespace

std::optional<PanelColorValues> edit_panel_colors(const PanelColorValues &current) {
  PanelColorSet values;
  values.values = current.values;
  const auto chosen = edit_panel_colors_internal(values);
  if (!chosen) return std::nullopt;
  return PanelColorValues{chosen->values};
}

const char *options_text() { return editor_options_text(); }

PanelColorsLayout panel_colors_layout() {
  fl_font(FL_HELVETICA, FL_NORMAL_SIZE);
  int label_width{};
  for (const auto *name : color_names) {
    const auto label = std::string(name) + " #RRGGBB";
    label_width = std::max(label_width, static_cast<int>(fl_width(label.c_str())));
  }
  PanelColorsLayout layout;
  layout.label_width = label_width;
  layout.input_x = 15 + label_width + 12;
  layout.button_x = layout.input_x + input_width + 10;
  layout.window_width = layout.button_x + button_width + 15;
  return layout;
}

void EditorApp::load_appearance(std::optional<AppearanceLoad> startup_appearance) {
  auto loaded = startup_appearance ? std::move(*startup_appearance) : load_platform_appearance();
  appearance_ = loaded.appearance;
  appearance_error_ = std::move(loaded.error);
  // The default is a preset too, so it is always applied.
  apply_editor_appearance(appearance_, window, profiles, preview);
}


void EditorApp::refresh_desktop_appearance() {
  reapply_editor_desktop_appearance(window, profiles, preview);
}

void EditorApp::open_profiles_folder() {
  const auto directory = session.directory();
  if (directory.empty()) { report(StatusKind::failure, profile_directory_unavailable_reason()); return; }
  if (opener_.running()) { report(StatusKind::information, "The file manager is still starting."); return; }
  const std::string path = directory.string();
  if (!opener_.start(directory)) {
    report(StatusKind::failure, "Could not start " + std::string(folder_opener_name()) +
           "; the profiles folder is " + path);
    return;
  }
  opener_path_ = path;
  report(StatusKind::information, "Opening the profiles folder: " + path);
  Fl::add_timeout(0.1, &EditorApp::opener_tick, this);
}

void EditorApp::opener_tick(void *data) {
  auto &app = *static_cast<EditorApp *>(data);
  if (!app.opener_.running()) return;
  const auto exit = app.opener_.poll();
  if (!exit) { Fl::repeat_timeout(0.1, &EditorApp::opener_tick, data); return; }
  if (exit->accepted_without_process)
    app.report(StatusKind::information, std::string(folder_opener_name()) +
               " accepted the folder request, but no process exit status is available: " +
               app.opener_path_);
  else if (!exit->code || *exit->code != 0)
    app.report(StatusKind::failure, std::string(folder_opener_name()) +
               " could not open the profiles folder (" +
               (exit->code ? "exit " + std::to_string(*exit->code)
                           : std::string("no exit status")) +
               "); it is " + app.opener_path_);
  else
    app.report(StatusKind::success, "Opened the profiles folder: " + app.opener_path_);
}

void EditorApp::stop_opener() {
  Fl::remove_timeout(&EditorApp::opener_tick, this);
  opener_.stop();
}

} // namespace layerc::editor
