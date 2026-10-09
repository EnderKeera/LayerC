#include "editor_model.hpp"
#include "profile_edit.hpp"
#include "layerc/core/version.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {
namespace editor = layerc::editor;

struct App;
class Preview final : public Fl_Widget {
public:
  Preview(int x, int y, int w, int h, App &app)
      : Fl_Widget(x, y, w, h, "Panel preview"), app_(app) {}
  void draw() override;
private:
  App &app_;
};

struct App {
  editor::LinuxEditorModel model;
  Fl_Double_Window window{900, 765, "LayerE — Linux X11 vertical slice"};
  Fl_Hold_Browser *list{};
  Fl_Group *fields{};
  Fl_Input *name{};
  Fl_Input *executable{};
  Fl_Check_Button *enabled{};
  Fl_Input *cooldown_name{};
  Fl_Input *duration{};
  Fl_Input *panel_x{};
  Fl_Input *panel_y{};
  Fl_Box *save_status{};
  Fl_Box *daemon_status{};
  Fl_Box *accessibility_status{};
  Preview *preview{};
  Fl_Button *save_button{};
  bool dirty_controls{};
  int selected_row{};
  std::string daemon_text;
  std::string save_text;
  editor::SliceFields values() const {
    return {name->value(), executable->value(), enabled->value() != 0,
            cooldown_name->value(), duration->value(), panel_x->value(), panel_y->value()};
  }
  void report(std::string text) {
    save_text = std::move(text);
    save_status->copy_label(save_text.c_str());
    save_status->redraw();
  }
  void query_status() {
    const auto status = editor::query_daemon_status();
    daemon_text = status.summary();
    for (const auto &capability : status.capabilities) {
      daemon_text += "\n" + capability.name + ": " + capability.availability;
      if (!capability.reasons.empty()) daemon_text += " (" + capability.reasons + ")";
    }
    daemon_status->copy_label(daemon_text.c_str());
    daemon_status->redraw();
  }
  void refresh_fields() {
    const auto value = model.fields();
    name->value(value.name.c_str());
    executable->value(value.executable.c_str());
    enabled->value(value.enabled ? 1 : 0);
    cooldown_name->value(value.cooldown_name.c_str());
    duration->value(value.duration_seconds.c_str());
    panel_x->value(value.panel_x.c_str());
    panel_y->value(value.panel_y.c_str());
    fields->activate();
    save_button->activate();
    // Child deactivation is independent of the group's activation flag.
    // Restore the fields before applying the newly selected profile's limits.
    for (auto *input : {cooldown_name, duration, panel_x, panel_y})
      input->activate();
    if (model.document().profile().cooldowns.empty()) {
      cooldown_name->deactivate();
      duration->deactivate();
    }
    if (model.document().profile().panels.empty()) {
      panel_x->deactivate();
      panel_y->deactivate();
    }
    dirty_controls = false;
    preview->redraw();
  }
  void fill_list() {
    list->clear();
    selected_row = 0;
    int index = 0;
    for (const auto &row : model.profiles().profiles) {
      ++index;
      if (model.has_document() && row.path == model.document().path())
        selected_row = index;
      const auto label = editor::profile_list_text(row.profile);
      list->add(label.c_str());
    }
    list->value(selected_row);
  }
  bool save() {
    const auto result = model.save(values(), [] {
      return fl_choice("Upgrade this legacy profile to the current format?\n"
                       "Older LayerC versions may no longer read it.",
                       "Cancel", "Upgrade", nullptr) == 1;
    });
    report(result.text);
    if (result.saved()) {
      dirty_controls = false;
      fill_list();
    }
    query_status();
    return result.saved();
  }
  bool may_leave() {
    if (!dirty_controls && !model.document().dirty()) return true;
    const auto choice = fl_choice("This profile has unsaved changes.",
                                  "Cancel", "Discard", "Save");
    return choice == 1 || (choice == 2 && save());
  }
};

void Preview::draw() {
  fl_push_clip(x(), y(), w(), h());
  fl_rectf(x(), y(), w(), h(), fl_rgb_color(31, 36, 41));
  if (app_.model.has_document()) {
    const auto &profile = app_.model.document().profile();
    fl_font(FL_HELVETICA, 12);
    for (const auto &panel : profile.panels) {
      const int px = x() + static_cast<int>(panel.anchor_x * w()) - 85;
      int py = y() + static_cast<int>(panel.anchor_y * h());
      for (const auto &id : panel.cooldown_ids) {
        fl_rectf(px, py, 170, 18, fl_rgb_color(80, 186, 200));
        fl_color(FL_WHITE);
        const auto *text = &id;
        for (const auto &cooldown : profile.cooldowns)
          if (cooldown.id == id) { text = &cooldown.name; break; }
        fl_draw(text->c_str(), px + 4, py + 13);
        py += 22;
      }
    }
  }
  fl_pop_clip();
}

void field_changed(Fl_Widget *, void *data) {
  auto &app = *static_cast<App *>(data);
  app.dirty_controls = true;
  if (const auto error = app.model.apply(app.values()))
    app.report("Invalid input: " + *error);
  else
    app.report("Unsaved changes.");
  app.preview->redraw();
}

void profile_selected(Fl_Widget *, void *data) {
  auto &app = *static_cast<App *>(data);
  const auto row = app.list->value();
  if (row <= 0 || row == app.selected_row) return;
  const auto index = static_cast<std::size_t>(row - 1);
  if (index >= app.model.profiles().profiles.size()) return;
  const auto target = app.model.profiles().profiles[index].path;
  if (!app.may_leave()) { app.list->value(app.selected_row); return; }
  if (const auto error = app.model.open_path(target)) {
    app.list->value(app.selected_row);
    app.report(*error);
    return;
  }
  app.fill_list();
  app.refresh_fields();
  app.report("Profile loaded. Explicit Save applies changes.");
}

void build(App &app) {
  app.window.begin();
  app.list = new Fl_Hold_Browser(15, 35, 230, 400, "Profiles");
  app.list->align(FL_ALIGN_TOP_LEFT);
  app.list->format_char(0); // Authored profile labels are plain UTF-8 text.
  app.list->callback(profile_selected, &app);
  app.fields = new Fl_Group(260, 15, 625, 330);
  app.fields->begin();
  auto input = [&](const int x, const int y, const int width, const char *label) {
    auto *widget = new Fl_Input(x, y, width, 30, label);
    widget->align(FL_ALIGN_TOP_LEFT);
    widget->when(FL_WHEN_CHANGED);
    widget->callback(field_changed, &app);
    return widget;
  };
  app.name = input(270, 40, 280, "Profile name");
  app.executable = input(580, 40, 295, "Executable name");
  app.enabled = new Fl_Check_Button(270, 85, 250, 30, "Profile enabled");
  app.enabled->callback(field_changed, &app);
  app.cooldown_name = input(270, 150, 280, "First cooldown name");
  app.duration = input(580, 150, 295, "Duration (seconds)");
  app.panel_x = input(270, 220, 280, "First panel X (0–1)");
  app.panel_y = input(580, 220, 295, "First panel Y (0–1)");
  auto *notice = new Fl_Box(270, 280, 605, 50,
      "Vertical slice: edits the first cooldown and panel only.\n"
      "Other bindings, panels and valid modules are retained.");
  notice->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  app.fields->end();
  app.preview = new Preview(260, 345, 625, 120, app);
  app.save_button = new Fl_Button(15, 450, 105, 35, "&Save");
  app.save_button->shortcut(FL_CTRL | 's');
  app.save_button->callback([](Fl_Widget *, void *data) {
    (void)static_cast<App *>(data)->save();
  }, &app);
  auto *refresh = new Fl_Button(135, 450, 110, 35, "Daemon status");
  refresh->callback([](Fl_Widget *, void *data) {
    static_cast<App *>(data)->query_status();
  }, &app);
  app.save_status = new Fl_Box(15, 495, 870, 45);
  app.save_status->box(FL_FLAT_BOX);
  app.save_status->color(app.window.color());
  app.save_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  app.daemon_status = new Fl_Box(15, 545, 870, 140);
  app.daemon_status->box(FL_FLAT_BOX);
  app.daemon_status->color(app.window.color());
  app.daemon_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  app.daemon_status->labelsize(11);
  app.accessibility_status = new Fl_Box(15, 700, 870, 35,
      "Accessibility unavailable: production bridge integration is not complete.");
  app.accessibility_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  app.accessibility_status->labelsize(11);
  app.window.end();
  app.window.size_range(900, 765);
  app.window.resizable(app.preview);
  app.window.callback([](Fl_Widget *, void *data) {
    auto &state = *static_cast<App *>(data);
    if (state.may_leave()) state.window.hide();
  }, &app);
  app.fill_list();
  if (!app.model.profiles().profiles.empty()) {
    if (const auto error = app.model.open(0)) app.report(*error);
    else {
      app.selected_row = 1;
      app.list->value(1);
      app.refresh_fields();
    }
  }
  if (!app.model.has_document()) {
    app.fields->deactivate();
    app.save_button->deactivate();
    auto reason = app.model.profiles().error;
    if (reason.empty()) reason = app.model.profiles().warning_text();
    app.report(reason.empty()
        ? "No readable profiles. Import a profile with layerctl to begin."
        : "No readable profiles: " + reason);
  } else if (!app.model.profiles().warning_text().empty()) {
    app.report(app.model.profiles().warning_text());
  } else {
    app.report("Profile loaded. Explicit Save applies changes.");
  }
  app.query_status();
}

} // namespace

int main(const int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::puts("layere — experimental Linux X11 editor vertical slice\n"
              "Uses the normal XDG profile store. --help, --version.");
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::puts(layerc::core::product_version.data());
    return 0;
  }
  if (argc != 1) {
    std::fputs("Unknown option; use layere --help.\n", stderr);
    return 64;
  }
  const auto *display = std::getenv("DISPLAY");
  if (!display || !*display) {
    std::fputs("LayerE requires an X11 display; Wayland is not supported.\n", stderr);
    return 2;
  }
  // FLTK 1.4's hybrid library otherwise prefers native Wayland when a
  // compositor is available. Select X11 before any window opens, including
  // when the ambient FLTK_BACKEND requests the unsupported backend.
  // https://www.fltk.org/doc-1.4/osissues.html#osissues_wl_x11_hybrid
  if (::setenv("FLTK_BACKEND", "x11", 1) != 0) {
    std::fputs("LayerE could not select its required X11 backend.\n", stderr);
    return 2;
  }
  App app;
  build(app);
  app.window.show();
  return Fl::run();
}
