// Tools and file commands beyond the core edit loop: Save As, the profiles
// folder, Split grouped bars, panel colours, Options and editor Appearance.
#include "editor_tools.hpp"
#include "accessible_widgets.hpp"
#include "editor_app.hpp"
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

#include <array>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

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

constexpr const char *options_dialog_text =
    "Exclude editor windows from screen capture\n"
    "  Unavailable on Linux X11: the X server offers no per-window capture\n"
    "  exclusion, so any X client with access to the display can read them.\n\n"
    "Isolated visual recognition\n"
    "  Unavailable on Linux: LayerD runs visual recognition in its own process\n"
    "  through the bounded X11 capture; there is no separate worker to isolate.\n"
    "  A profile's Windows isolated-runtime setting is preserved unchanged.\n\n"
    "Advanced settings\n"
    "  LayerE on Linux always shows every field that the Windows editor keeps\n"
    "  behind Show Advanced Settings (IDs, charges, retrigger, recovery).";

} // namespace

std::optional<PanelColorValues> edit_panel_colors(const PanelColorValues &current) {
  PanelColorSet values;
  values.values = current.values;
  const auto chosen = edit_panel_colors_internal(values);
  if (!chosen) return std::nullopt;
  return PanelColorValues{chosen->values};
}

const char *options_text() { return options_dialog_text; }

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

void EditorApp::load_appearance() {
  appearance_path_ = editor_appearance_path();
  auto loaded = load_editor_appearance(appearance_path_);
  appearance_ = loaded.appearance;
  appearance_error_ = std::move(loaded.error);
  // The default is a preset too, so it is always applied.
  apply_editor_appearance(appearance_, window, profiles, preview);
}


void EditorApp::open_profiles_folder() {
  const auto directory = session.directory();
  if (directory.empty()) { report("No profiles folder: HOME is not an absolute path."); return; }
  if (opener_ > 0) { report("The file manager is still starting."); return; }
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  // Its own session: closing LayerE never signals the file manager. Default
  // signal dispositions and an empty mask, whatever LayerE changed.
  sigset_t none, all;
  sigemptyset(&none);
  sigfillset(&all);
  posix_spawnattr_setsigmask(&attributes, &none);
  posix_spawnattr_setsigdefault(&attributes, &all);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK |
                                            POSIX_SPAWN_SETSIGDEF);
  // Only stdio is inherited: no X11, control-socket or notification fds.
  posix_spawn_file_actions_t files;
  posix_spawn_file_actions_init(&files);
  posix_spawn_file_actions_addopen(&files, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addclosefrom_np(&files, STDERR_FILENO + 1);
  const std::string path = directory.string();
  char program[] = "xdg-open";
  std::array<char *, 3> argv{program, const_cast<char *>(path.c_str()), nullptr};
  pid_t child{};
  const int result = posix_spawnp(&child, program, &files, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&files);
  posix_spawnattr_destroy(&attributes);
  if (result != 0) {
    report("Could not start xdg-open; the profiles folder is " + path);
    return;
  }
  opener_ = child;
  opener_path_ = path;
  report("Opening the profiles folder: " + path);
  Fl::add_timeout(0.1, &EditorApp::opener_tick, this);
}

void EditorApp::opener_tick(void *data) {
  auto &app = *static_cast<EditorApp *>(data);
  if (app.opener_ <= 0) return;
  int status{};
  const pid_t done = waitpid(app.opener_, &status, WNOHANG);
  if (done == 0) { Fl::repeat_timeout(0.1, &EditorApp::opener_tick, data); return; }
  app.opener_ = 0;
  if (done < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    app.report("xdg-open could not open the profiles folder (" +
               (done > 0 && WIFEXITED(status) ? "exit " + std::to_string(WEXITSTATUS(status))
                                              : std::string("no exit status")) +
               "); it is " + app.opener_path_);
  else
    app.report("Opened the profiles folder: " + app.opener_path_);
}

void EditorApp::stop_opener() {
  Fl::remove_timeout(&EditorApp::opener_tick, this);
  if (opener_ > 0) {
    // Reap without blocking; an unfinished launcher is left to its session.
    (void)waitpid(opener_, nullptr, WNOHANG);
    opener_ = 0;
  }
}

} // namespace layerc::editor
