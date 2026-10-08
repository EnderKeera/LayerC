// FLTK 1.4 prototype of the Linux editor's representative UI (Change 1).
#include "proto_common.hpp"
#ifdef LAYERC_PROTO_ATSPI
#include "fltk_a11y.hpp"
#endif

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Tabs.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <chrono>
#include <cmath>
#include <string>

namespace {
using layerc::prototype::Model;

struct App;

class Preview final : public Fl_Widget {
public:
  Preview(int x, int y, int w, int h, App &app) : Fl_Widget(x, y, w, h), app_(app) {}
  void draw() override;

private:
  App &app_;
};

class ProfileBrowser final : public Fl_Hold_Browser {
public:
  ProfileBrowser(int x, int y, int w, int h, Fl_Menu_Button &menu)
      : Fl_Hold_Browser(x, y, w, h), menu_(menu) {}
  int handle(int event) override {
    if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
      menu_.position(Fl::event_x(), Fl::event_y());
      menu_.popup();
      return 1;
    }
    return Fl_Hold_Browser::handle(event);
  }

private:
  Fl_Menu_Button &menu_;
};

// Reverse Tab entering the tabs reaches the visible page's last control,
// rather than stopping on the header and then skipping the page entirely.
class EditorTabs final : public Fl_Tabs {
public:
  using Fl_Tabs::Fl_Tabs;
  int handle(int event) override {
    if (event == FL_FOCUS && Fl::event() == FL_KEYBOARD && Fl::event_key() == FL_Tab &&
        Fl::event_shift())
      return Fl_Group::handle(event);
    return Fl_Tabs::handle(event);
  }
};

struct App {
  Model model;
  Fl_Double_Window *window{};
  ProfileBrowser *list{};
  Fl_Hold_Browser *cooldowns{};
  Fl_Input *name{};
  Fl_Input *executable{};
  Fl_Input *cooldown_name{};
  Fl_Spinner *duration{};
  Fl_Spinner *charges{};
  Fl_Box *binding{};
  Preview *preview{};
  Fl_Box *status{};
  std::string status_text{"Ready"};
  std::string binding_text;
  int cooldown{};
  layerc::prototype::CycleDriver cycles;

  void set_status(std::string text) {
    status_text = std::move(text);
    status->label(status_text.c_str());
    status->redraw();
  }
};

void Preview::draw() {
  fl_rectf(x(), y(), w(), h(), fl_rgb_color(31, 36, 41));
  const auto &profile = app_.model.document().profile();
  fl_font(FL_HELVETICA, 11);
  for (const auto &panel : profile.panels) {
    const int px = x() + static_cast<int>(panel.anchor_x * w()) - 90;
    int py = y() + static_cast<int>(panel.anchor_y * h()) - 10;
    for (const auto &id : panel.cooldown_ids) {
      fl_rectf(px, py, 180, 14, fl_rgb_color(80, 186, 200));
      fl_color(fl_rgb_color(241, 245, 247));
      fl_draw(id.c_str(), px + 4, py + 11);
      py += 16;
    }
  }
}

void refresh_cooldown(App &app) {
  const auto &profile = app.model.document().profile();
  if (app.cooldown < 0 || static_cast<std::size_t>(app.cooldown) >= profile.cooldowns.size())
    return;
  const auto &cooldown = profile.cooldowns[static_cast<std::size_t>(app.cooldown)];
  app.cooldown_name->value(cooldown.name.c_str());
  app.duration->value(static_cast<double>(cooldown.duration.count()) / 1000.0);
  app.charges->value(cooldown.max_charges);
  app.binding_text = layerc::prototype::binding_text(profile, cooldown.id);
  app.binding->label(app.binding_text.c_str());
  app.binding->redraw();
}

void refresh_document(App &app) {
  const auto &profile = app.model.document().profile();
  app.name->value(profile.name.c_str());
  app.executable->value(profile.game.executable_name.c_str());
  app.cooldowns->clear();
  for (const auto &cooldown : profile.cooldowns) app.cooldowns->add(cooldown.name.c_str());
  app.cooldown = 0;
  app.cooldowns->value(1);
  refresh_cooldown(app);
  app.preview->redraw();
}

void select_profile(App &app, const int line) {
  if (line <= 0) return;
  app.list->value(line);
  if (app.model.open(static_cast<std::size_t>(line - 1))) {
    refresh_document(app);
    app.set_status("Profile loaded.");
  } else {
    app.set_status("Could not read the selected profile.");
  }
}

void on_profile(Fl_Widget *, void *data) {
  auto &app = *static_cast<App *>(data);
  select_profile(app, app.list->value());
}

void on_cooldown(Fl_Widget *, void *data) {
  auto &app = *static_cast<App *>(data);
  if (app.cooldowns->value() <= 0) return;
  app.cooldown = app.cooldowns->value() - 1;
  refresh_cooldown(app);
}

void on_field(Fl_Widget *, void *data) {
  auto &app = *static_cast<App *>(data);
  auto &document = app.model.document();
  auto &profile = document.profile();
  profile.name = app.name->value();
  profile.game.executable_name = app.executable->value();
  if (app.cooldown >= 0 && static_cast<std::size_t>(app.cooldown) < profile.cooldowns.size()) {
    auto &cooldown = profile.cooldowns[static_cast<std::size_t>(app.cooldown)];
    cooldown.name = app.cooldown_name->value();
    cooldown.duration =
        layerc::core::Milliseconds(std::llround(app.duration->value() * 1000.0));
    cooldown.max_charges = static_cast<std::uint32_t>(app.charges->value());
  }
  (void)document.history().record(profile, document.visual(), document.automation(), true);
  document.set_dirty(true);
  app.preview->redraw();
}

// The one validation dialog path, shared by Save failures and the cycles.
// The one validation dialog, shared by Save failures and the cycle workload.
// draw() records that the dialog was actually rendered.
class ValidationDialog final : public Fl_Double_Window {
public:
  explicit ValidationDialog(const std::string &text) : Fl_Double_Window(360, 120, "Validation") {
    auto *message = new Fl_Box(12, 10, 336, 62);
    message->copy_label(text.c_str());
    message->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    auto *ok = new Fl_Return_Button(258, 82, 90, 28, "OK");
    ok->callback([](Fl_Widget *widget, void *) { widget->window()->hide(); });
    end();
    set_modal();
  }
  void draw() override {
    Fl_Double_Window::draw();
    drawn_ = true;
  }
  [[nodiscard]] bool drawn() const noexcept { return drawn_; }

private:
  bool drawn_{};
};

// Runs the modal dialog until it closes. For the cycle workload it is closed
// as soon as it has rendered, bounded by two seconds; returns whether it drew.
bool show_validation(const std::string &text, const bool dismiss_after_draw = false) {
  ValidationDialog dialog(text);
  dialog.show();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (dialog.shown()) {
    Fl::wait(0.005);
    if (dismiss_after_draw && (dialog.drawn() || std::chrono::steady_clock::now() >= deadline))
      dialog.hide();
  }
  return dialog.drawn();
}

void on_save(Fl_Widget *, void *data) {
  auto &app = *static_cast<App *>(data);
  const auto outcome = app.model.save();
  app.set_status(outcome);
  if (outcome != "Saved") (void)show_validation(outcome);
}

void on_menu(Fl_Widget *widget, void *data) {
  auto &app = *static_cast<App *>(data);
  const auto *item = static_cast<Fl_Menu_Button *>(widget)->mvalue();
  if (item != nullptr) app.set_status(std::string("Menu: ") + item->label());
}

// Capture is explicitly armed and confined to this focused editor control.
// It displays the local toolkit key only; production binding persistence is
// deliberately outside this representative-UI experiment.
class CaptureButton final : public Fl_Button {
public:
  CaptureButton(int x, int y, int w, int h, App &app)
      : Fl_Button(x, y, w, h, "C&apture key"), app_(app) {
    callback([](Fl_Widget *widget, void *) {
      auto &button = *static_cast<CaptureButton *>(widget);
      button.capturing_ = !button.capturing_;
      button.app_.set_status(button.capturing_ ? "Press a focused key; Escape or Tab cancels." : "Capture cancelled.");
    });
  }
  int handle(int event) override {
    if (capturing_ && Fl::focus() == this && (event == FL_KEYDOWN || event == FL_SHORTCUT)) {
      const int key = Fl::event_key();
      if (key == FL_Tab || key == FL_Escape) {
        capturing_ = false;
        app_.set_status("Capture cancelled.");
        if (key == FL_Escape) return 1;
      } else if (key == FL_Shift_L || key == FL_Shift_R || key == FL_Control_L ||
                 key == FL_Control_R || key == FL_Alt_L || key == FL_Alt_R) {
        return 1;
      } else {
        capturing_ = false;
        app_.binding_text = "Key " + std::to_string(key);
        app_.binding->label(app_.binding_text.c_str()); app_.binding->redraw();
        app_.set_status("Captured focused key (prototype display only).");
        return 1;
      }
    }
    return Fl_Button::handle(event);
  }
private:
  App &app_;
  bool capturing_{};
};

void cycle_step(void *data) {
  auto &app = *static_cast<App *>(data);
  using Step = layerc::prototype::CycleDriver::Step;
  const auto step = app.cycles.next();
  if (step == Step::done) return;
  if (step == Step::pause) {
    Fl::add_timeout(layerc::prototype::CycleDriver::pause_seconds, cycle_step, data);
    return;
  }
  const auto rows = static_cast<int>(app.model.rows().size());
  if (rows != 0) select_profile(app, static_cast<int>(app.model.selected() + 1) % rows + 1);
  // A dialog that never renders stops the workload; the harness then fails.
  if (!show_validation("Validation", true)) {
    layerc::prototype::announce("cycle-dialog-unpainted");
    return;
  }
  app.cycles.completed();
  Fl::add_timeout(layerc::prototype::CycleDriver::cycle_seconds, cycle_step, data);
}

} // namespace

int main(int argc, char **argv) {
  App app;
  if (!app.model.scan()) return 64;
  if (app.cycles.active() && app.model.rows().empty()) {
    std::fprintf(stderr, "Prototype refused: cycle workload requires a populated fixture\n");
    return 65;
  }
  Fl::set_font(FL_HELVETICA, "DejaVu Sans");
  Fl::scheme("gtk+");
  app.window = new Fl_Double_Window(900, 600, "LayerC Editor (FLTK prototype)");
  auto *menu = new Fl_Menu_Button(0, 0, 0, 0);
  menu->type(Fl_Menu_Button::POPUP3);
  menu->clear_visible_focus();  // Pointer popup only; never an invisible focus stop.
  menu->add("&New profile", 0, on_menu, &app);
  menu->add("&Duplicate", 0, on_menu, &app);
  menu->add("De&lete", 0, on_menu, &app);
  app.list = new ProfileBrowser(8, 8, 240, 584, *menu);
  app.list->label("Profiles");
  app.list->align(FL_ALIGN_TOP_LEFT);
  for (const auto &row : app.model.rows()) app.list->add(row.label.c_str());
  app.list->callback(on_profile, &app);

  auto *tabs = new EditorTabs(256, 8, 636, 548);
  auto *profile_tab = new Fl_Group(256, 33, 636, 523, "Profile");
  app.name = new Fl_Input(360, 45, 520, 25, "&Name");
  app.executable = new Fl_Input(360, 75, 520, 25, "&Application");
  // Stretch spare space below the fields, not their fixed row heights.
  profile_tab->resizable(new Fl_Box(360, 105, 520, 441));
  profile_tab->end();
  auto *cooldown_tab = new Fl_Group(256, 33, 636, 523, "Cooldowns");
  app.cooldowns = new Fl_Hold_Browser(264, 45, 180, 500);
  app.cooldowns->callback(on_cooldown, &app);
  app.cooldown_name = new Fl_Input(560, 45, 320, 25, "Cooldown &name");
  app.duration = new Fl_Spinner(560, 75, 120, 25, "&Duration (s)");
  app.duration->type(FL_FLOAT_INPUT);
  app.duration->range(0.001, 86400);
  app.duration->step(0.1);
  app.charges = new Fl_Spinner(560, 105, 120, 25, "&Charges");
  app.charges->range(1, 99);
  app.binding = new Fl_Box(560, 135, 320, 25);
  app.binding->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  new CaptureButton(560, 165, 120, 25, app);
  // Keep numeric controls readable while the name and list absorb resizing.
  cooldown_tab->resizable(new Fl_Box(700, 200, 180, 346));
  cooldown_tab->end();
  auto *panel_tab = new Fl_Group(256, 33, 636, 523, "Panels");
  app.preview = new Preview(264, 41, 620, 507, app);
  panel_tab->resizable(app.preview);
  panel_tab->end();
  tabs->end();
  for (Fl_Widget *field : {static_cast<Fl_Widget *>(app.name), static_cast<Fl_Widget *>(app.executable),
                           static_cast<Fl_Widget *>(app.cooldown_name),
                           static_cast<Fl_Widget *>(app.duration),
                           static_cast<Fl_Widget *>(app.charges)}) {
    field->callback(on_field, &app);
    field->when(FL_WHEN_CHANGED);
  }
  // The status absorbs width changes; Save keeps a readable fixed size.
  auto *bar = new Fl_Group(256, 564, 636, 28);
  app.status = new Fl_Box(256, 564, 540, 28, "Ready");
  app.status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  auto *save = new Fl_Button(804, 564, 88, 28, "&Save");
  save->callback(on_save, &app);
  bar->resizable(app.status);
  bar->end();
  app.window->resizable(tabs);
  app.window->size_range(760, 400);
  app.window->end();
  app.window->show(argc, argv);
  if (!app.model.rows().empty()) select_profile(app, 1);
#ifdef LAYERC_PROTO_ATSPI
  layerc::prototype::AccessibilityBridge bridge(*app.window);
  if (!bridge.available()) app.set_status("Accessibility unavailable: " + bridge.fault());
  layerc::prototype::announce(bridge.available() ? "a11y=available" : "a11y=unavailable");
#endif
  app.model.announce_fixture();
  layerc::prototype::announce("ready");
  if (app.cycles.active())
    Fl::add_timeout(layerc::prototype::CycleDriver::cycle_seconds, cycle_step, &app);
  return Fl::run();
}
