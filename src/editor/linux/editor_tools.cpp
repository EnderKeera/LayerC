// Tools and file commands beyond the core edit loop: Save As, the profiles
// folder, Split grouped bars, panel colours, Options and editor Appearance.
#include "editor_app.hpp"
#include "appearance_window.hpp"
#include "cooldown_groups_window.hpp"
#include "diagnostics_window.hpp"
#include "profile_edit.hpp"
#include "profile_mutations.hpp"

#include <FL/Fl.H>
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

// Modal panel colours: a #RRGGBB field and a chooser per bar colour.
std::optional<PanelColorSet> edit_panel_colors(const PanelColorSet &current) {
  Fl_Double_Window dialog(470, 250, "Panel colours — LayerE");
  std::array<Fl_Input *, 4> fields{};
  struct Pick { Fl_Input *field; const char *title; };
  std::array<Pick, 4> picks{};
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const int y = 15 + static_cast<int>(index) * 40;
    fields[index] = new Fl_Input(150, y, 140, 28, nullptr);
    fields[index]->copy_label((std::string(color_names[index]) + " #RRGGBB").c_str());
    fields[index]->value(format_rgb(current.values[index]).c_str());
    picks[index] = {fields[index], color_names[index]};
    auto *choose = new Fl_Button(300, y, 155, 28, nullptr);
    choose->copy_label(("Choose " + std::string(color_names[index]) + "...").c_str());
    choose->callback([](Fl_Widget *, void *data) {
      auto &pick = *static_cast<Pick *>(data);
      const auto initial = parse_rgb(pick.field->value()).value_or(0x808080);
      if (const auto color = choose_color(pick.title, initial))
        pick.field->value(format_rgb(*color).c_str());
    }, &picks[index]);
  }
  auto *message = new Fl_Box(15, 170, 440, 28);
  message->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  bool accepted = false;
  auto *cancel = new Fl_Button(220, 205, 110, 30, "Cancel");
  cancel->callback([](Fl_Widget *widget, void *) { widget->window()->hide(); });
  struct Accept { bool *accepted; std::array<Fl_Input *, 4> *fields; Fl_Box *message; };
  Accept accept_state{&accepted, &fields, message};
  auto *ok = new Fl_Button(345, 205, 110, 30, "OK");
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

constexpr const char *options_text =
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

void EditorApp::load_appearance() {
  appearance_path_ = editor_appearance_path();
  auto loaded = load_editor_appearance(appearance_path_);
  appearance_ = loaded.appearance;
  appearance_error_ = std::move(loaded.error);
  if (appearance_ != EditorAppearance{} )
    apply_editor_appearance(appearance_, window, profiles, preview);
}

bool EditorApp::tools_command(const EditorCommand action) {
  switch (action) {
  case EditorCommand::cooldown_groups: {
    if (!session.has_document() || !apply_fields()) return true;
    CooldownGroupsDialog dialog(session);
    apply_editor_appearance(appearance_, dialog, nullptr, nullptr);
    dialog.show();
    while (dialog.shown()) Fl::wait();
    if (dialog.applied) changed(std::nullopt);
    else report("Cooldown groups unchanged.");
    return true;
  }
  case EditorCommand::options:
    show_text_dialog("Options — LayerE", options_text);
    return true;
  case EditorCommand::appearance: {
    if (!appearance_error_.empty() &&
        fl_choice("%s\nApplying a new choice replaces the file.", "Cancel", "Continue", nullptr,
                  appearance_error_.c_str()) != 1)
      return true;
    const auto chosen = edit_editor_appearance(appearance_, window, profiles, preview);
    if (!chosen) { report("Appearance unchanged."); return true; }
    appearance_ = *chosen;
    if (const auto error = save_editor_appearance(appearance_path_, appearance_)) {
      report(*error + " The choice applies until LayerE closes.");
    } else {
      appearance_error_.clear();
      report("Appearance saved: " + std::string(theme_label(appearance_.theme)) + ".");
    }
    return true;
  }
  case EditorCommand::open_folder: {
    open_profiles_folder();
    return true;
  }
  case EditorCommand::split_bars: {
    if (!apply_fields()) return true;
    std::optional<std::size_t> first;
    const auto error = session.edit([&](auto &profile) {
      first = split_grouped_bars(profile, preview->h());
    });
    if (error) { changed(error); return true; }
    if (!first) { report("Bars are already independent; nothing was split."); return true; }
    panel_id_ = session.document().profile().panels[*first].id;
    changed(std::nullopt);
    report("Split grouped bars into independently positioned cooldown bars. " + edit_status());
    return true;
  }
  case EditorCommand::panel_colors: {
    const auto index = panel_index();
    if (!index || !apply_fields()) return true;
    const auto &bar = session.document().profile().panels[*index].bar;
    const auto chosen = edit_panel_colors(
        {{bar.fill_color, bar.background_color, bar.border_color, bar.text_color}});
    if (!chosen) { report("Panel colours unchanged."); return true; }
    const auto id = panel_id_;
    changed(session.edit([&](auto &profile) {
      for (auto &panel : profile.panels) {
        if (panel.id != id) continue;
        panel.bar.fill_color = chosen->values[0];
        panel.bar.background_color = chosen->values[1];
        panel.bar.border_color = chosen->values[2];
        panel.bar.text_color = chosen->values[3];
      }
    }));
    return true;
  }
  case EditorCommand::save_as: {
    if (!session.has_document()) { report("Open a profile before Save As."); return true; }
    if (!apply_fields()) { report("Save As needs valid fields first."); return true; }
    const auto current = session.document().profile().name;
    const char *name = fl_input("Save the open profile, with its modules, as a new profile named:",
                                (current + " (copy)").c_str());
    if (!name) return true;
    if (std::string_view(name).empty()) { report("Save As needs a profile name."); return true; }
    const std::string chosen(name);
    if (const auto error = session.duplicate_current()) { report(*error); refresh(); return true; }
    if (const auto error = session.edit([&](auto &profile) { profile.name = chosen; })) {
      report(*error); refresh(); return true;
    }
    refresh();
    (void)save();
    return true;
  }
  default:
    return false;
  }
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
