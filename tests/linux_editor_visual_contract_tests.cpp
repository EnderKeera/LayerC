// Structural regression evidence for the two completion-audit repairs:
// the calibration form's full-precision layout and a black accent that is
// distinct from the preset. Rendered judgement stays with the lead; these
// cases only assert the widget contract on a private X11 display.
#include "appearance_window.hpp"
#include "calibration_window.hpp"
#include "diagnostics_window.hpp"
#include "editor_app.hpp"
#include "profile_edit.hpp"
#include "profile_mutations.hpp"
#include "linux_editor_test_access.hpp"
#include "test_support.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"

#include <sys/stat.h>

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace editor = layerc::editor;
namespace core = layerc::core;
namespace platform = layerc::platform;
using layerc::test::expect;

namespace {

// First widget with exactly this label, through nested groups.
Fl_Widget *control(Fl_Group &group, const char *label) {
  for (int index = 0; index < group.children(); ++index) {
    auto *child = group.child(index);
    if (label != nullptr && child->label() != nullptr && std::string(child->label()) == label)
      return child;
    if (auto *nested = child->as_group())
      if (auto *found = control(*nested, label)) return found;
  }
  return nullptr;
}

// Visits every widget below `group`, depth first.
void visit(Fl_Group &group, const std::function<void(Fl_Widget &)> &visit_widget) {
  for (int index = 0; index < group.children(); ++index) {
    auto *child = group.child(index);
    visit_widget(*child);
    if (auto *nested = child->as_group()) visit(*nested, visit_widget);
  }
}

// Private store matching the widget suite's fixture environment: the main
// window needs one open profile before its demand-created pages exist.
class PrivateStore final {
public:
  PrivateStore()
      : temp_("layerc-editor-visual-contract"),
        home_("HOME", temp_.path()),
        config_("XDG_CONFIG_HOME", temp_.path() / "config"),
        data_("XDG_DATA_HOME", temp_.path() / "data"),
        runtime_("XDG_RUNTIME_DIR", temp_.path() / "runtime"),
        cache_("XDG_CACHE_HOME", temp_.path() / "cache"),
        state_("XDG_STATE_HOME", temp_.path() / "state"),
        override_("LAYERC_PROFILE_DIR", std::nullopt),
        store_override_("LAYERC_STORE_ROOT", std::nullopt),
        bus_("DBUS_SESSION_BUS_ADDRESS", std::nullopt),
        accessibility_bus_("AT_SPI_BUS_ADDRESS", std::nullopt),
        backend_("FLTK_BACKEND", std::filesystem::path("x11")) {
    layerc::test::require_x11();
    std::filesystem::create_directories(temp_.path() / "runtime");
    expect(::chmod((temp_.path() / "runtime").c_str(), 0700) == 0,
           "fixture control namespace is private");
    auto profile = editor::make_new_profile();
    profile.game.executable_name = "fixture.exe";
    expect(platform::write_profile_file(platform::profile_directory() / "fixture.json",
                                        core::serialize_profile_json(profile)).ok(),
           "store only a private fixture");
  }

private:
  layerc::test::TempDirectory temp_;
  layerc::test::ScopedEnvironment home_, config_, data_, runtime_, cache_, state_,
      override_, store_override_, bus_, accessibility_bus_, backend_;
};

// Drives the real modal once FLTK has mapped it, then stops retrying.
class ModalAction final {
public:
  explicit ModalAction(std::function<void(Fl_Window &)> action) : action_(std::move(action)) {
    Fl::add_timeout(.01, tick, this);
  }
  ~ModalAction() { Fl::remove_timeout(tick, this); }
  ModalAction(const ModalAction &) = delete;
  ModalAction &operator=(const ModalAction &) = delete;

  [[nodiscard]] bool delivered() const { return delivered_; }

  static void tick(void *data) {
    auto &self = *static_cast<ModalAction *>(data);
    if (auto *modal = Fl::modal()) {
      self.delivered_ = true;
      self.action_(*modal);
      return;
    }
    if (++self.attempts_ < 100) Fl::add_timeout(.01, tick, &self);
  }

private:
  std::function<void(Fl_Window &)> action_;
  bool delivered_{};
  int attempts_{};
};

[[nodiscard]] unsigned selection_rgb(const Fl_Widget &widget) {
  return Fl::get_color(widget.selection_color());
}

} // namespace

void linux_editor_calibration_layout_contracts() {
  layerc::test::ScopedEnvironment backend("FLTK_BACKEND", std::filesystem::path("x11"));
  layerc::test::require_x11();
  auto profile = editor::make_new_profile();
  auto seed = editor::CalibrationModel::open(profile, profile.cooldowns.front().id, std::nullopt);
  if (!expect(seed.has_value(), "the layout contract seeds from the selected cooldown")) return;
  const core::NormalizedRegion region{1.0 / 13.0, 2.0 / 13.0, 10.0 / 13.0, 9.0 / 13.0};

  bool inspected{};
  ModalAction action([&](Fl_Window &modal) {
    auto *x = dynamic_cast<Fl_Input *>(control(modal, "Region X (0–1)"));
    auto *y = dynamic_cast<Fl_Input *>(control(modal, "Region Y (0–1)"));
    auto *width = dynamic_cast<Fl_Input *>(control(modal, "Region width"));
    auto *height = dynamic_cast<Fl_Input *>(control(modal, "Region height"));
    auto *preview = dynamic_cast<editor::CalibrationPreview *>(control(modal, "Calibration preview"));
    if (!x || !y || !width || !height || !preview || !preview->selected) {
      expect(false, "the calibration modal owns the four region fields and the preview");
      modal.hide();
      return;
    }
    preview->selected(region);
    expect(std::string(x->value()) == editor::format_decimal(region.x) &&
               std::string(y->value()) == editor::format_decimal(region.y) &&
               std::string(width->value()) == editor::format_decimal(region.width) &&
               std::string(height->value()) == editor::format_decimal(region.height),
           "each region field keeps the exact full-precision decimal string");
    for (Fl_Input *field : {x, y, width, height}) {
      fl_font(field->textfont(), field->textsize());
      expect(fl_width(field->value()) + 12 <= field->w(),
             "a full-precision region value fits its field without clipping");
    }
    for (const char *label : {"Sample selection color", "Capture reference",
                              "Import PNG reference...", "Clear references"}) {
      auto *button = dynamic_cast<Fl_Button *>(control(modal, label));
      if (!button) {
        expect(false, "each calibration action keeps its exact label");
        modal.hide();
        return;
      }
      int label_width = 0, label_height = 0;
      button->measure_label(label_width, label_height);
      expect(label_width + 12 <= button->w(), "each calibration action label fits its button");
    }
    auto *references = dynamic_cast<Fl_Output *>(control(modal, "Template references"));
    auto *import = dynamic_cast<Fl_Button *>(control(modal, "Import PNG reference..."));
    auto *clear = dynamic_cast<Fl_Button *>(control(modal, "Clear references"));
    if (!references || !import || !clear) {
      expect(false, "the calibration modal owns the references output and its neighbours");
      modal.hide();
      return;
    }
    int references_label_width = 0, references_label = 0;
    references->measure_label(references_label_width, references_label);
    expect(references->y() - references_label >=
               std::max(import->y() + import->h(), clear->y() + clear->h()) + 4,
           "the references label owns a row clear of the action buttons");
    auto *outcome = dynamic_cast<Fl_Multiline_Output *>(control(modal, "Capture / live match status"));
    auto *semantics = dynamic_cast<Fl_Multiline_Output *>(control(modal, "Calibration semantics"));
    auto *tool = control(modal, "Preview tool");
    auto *notice = control(modal, "I acknowledge passive capture of the selected game window");
    auto *timeout = control(modal, "Input timeout (50–5000 ms)");
    auto *unavailable = control(modal, "Capture unavailable");
    if (!outcome || !semantics || !tool || !notice || !timeout || !unavailable) {
      expect(false, "the calibration modal owns the wrapped outputs and spacing controls");
      modal.hide();
      return;
    }
    expect(outcome->wrap() != 0 && semantics->wrap() != 0,
           "calibration status outputs wrap their text");
    int tool_label_width = 0, tool_label = 0;
    tool->measure_label(tool_label_width, tool_label);
    expect(tool->y() - tool_label >= notice->y() + notice->h(),
           "the preview-tool label clears the acknowledgement row");
    int semantics_label_width = 0, semantics_label = 0;
    semantics->measure_label(semantics_label_width, semantics_label);
    expect(semantics->y() - semantics_label >=
               std::max(timeout->y() + timeout->h(), unavailable->y() + unavailable->h()) + 4,
           "the semantics label clears the timeout row");
    int region_label_width = 0, width_label = 0, height_label = 0;
    width->measure_label(region_label_width, width_label);
    // measure_label treats the incoming width as a wrap limit; measure each
    // label unwrapped so the reported height is the real one-line label.
    region_label_width = 0;
    height->measure_label(region_label_width, height_label);
    const int previous_row = std::max(x->y() + x->h(), y->y() + y->h());
    expect(width->y() - width_label >= previous_row + 4 &&
               height->y() - height_label >= previous_row + 4,
           "the region width/height row keeps its labels clear of the row above");
    modal.size(640, 480);
    modal.redraw();
    Fl::check();
    Fl_Scroll *scroll = nullptr;
    for (int index = 0; index < modal.children(); ++index)
      if (auto *candidate = dynamic_cast<Fl_Scroll *>(modal.child(index))) scroll = candidate;
    if (!scroll) {
      expect(false, "the calibration body is a scroll group");
      modal.hide();
      return;
    }
    expect(scroll->scrollbar.visible() && scroll->hscrollbar.visible(),
           "a 640x480 window keeps the whole form reachable through both scrollbars");
    const auto move_axis = [](Fl_Scrollbar &bar, bool far_edge) {
      bar.value(far_edge ? bar.maximum() : bar.minimum());
      bar.do_callback();
      Fl::check();
    };
    move_axis(scroll->hscrollbar, true);
    move_axis(scroll->scrollbar, true);
    move_axis(scroll->hscrollbar, false);
    move_axis(scroll->scrollbar, false);
    expect(scroll->hscrollbar.minimum() == 0 && scroll->scrollbar.minimum() == 0,
           "scrolling back reaches the complete canvas origin, not the first widget");
    auto *target = control(modal, "Calibration target window");
    auto *predicate = control(modal, "Calibration predicate");
    if (!target || !predicate) {
      expect(false, "both first-row calibration controls remain present");
      modal.hide();
      return;
    }
    for (Fl_Widget *field : {target, predicate}) {
      int label_width = 0, label_height = 0;
      field->measure_label(label_width, label_height);
      expect(field->y() - label_height >= scroll->y() + 4,
             "returning to the top preserves the complete first-row label");
    }
    expect(target->x() >= scroll->x() + 4,
           "returning to the left preserves the form margin");
    move_axis(scroll->hscrollbar, true);
    move_axis(scroll->scrollbar, true);
    for (const char *label : {"Apply calibration", "Cancel calibration"}) {
      auto *button = control(modal, label);
      if (!button) {
        expect(false, "both calibration completion controls remain present");
        modal.hide();
        return;
      }
      expect(button->x() >= scroll->x() + 4 &&
                 button->x() + button->w() <= scroll->x() + scroll->w() - scroll->scrollbar.w() - 4 &&
                 button->y() >= scroll->y() + 4 &&
                 button->y() + button->h() <= scroll->y() + scroll->h() - scroll->hscrollbar.h() - 4,
             "scrolling to the far edge fully exposes Apply and Cancel with margins");
    }
    inspected = true;
    modal.hide();
  });
  const editor::CalibrationAccept accept =
      [](const editor::CalibrationDraft &) -> editor::CalibrationAcceptance {
    return std::string("unexpected apply");
  };
  const auto result = editor::run_x11_calibration({}, std::move(*seed), accept);
  expect(action.delivered() && inspected && !result,
         "the inspected calibration modal is dismissed without a draft");
}

void linux_editor_black_accent_dialog_contracts() {
  layerc::test::ScopedEnvironment backend("FLTK_BACKEND", std::filesystem::path("x11"));
  layerc::test::require_x11();
  Fl_Window root(400, 300);
  auto *list = new Fl_Hold_Browser(10, 10, 380, 280);
  root.end();
  editor::EditorAppearance initial;
  initial.theme = editor::EditorTheme::teal;

  bool painted_black{};
  ModalAction apply_black([&](Fl_Window &modal) {
    auto *accent = dynamic_cast<Fl_Input *>(control(modal, "Accent #RRGGBB"));
    auto *apply = dynamic_cast<Fl_Button *>(control(modal, "Apply"));
    if (!accent || !apply) {
      expect(false, "the appearance dialog owns the accent field and Apply");
      modal.hide();
      return;
    }
    accent->value("#000000");
    accent->do_callback();
    painted_black = selection_rgb(*list) == Fl::get_color(fl_rgb_color(24, 24, 24));
    apply->do_callback();
  });
  const auto black = editor::edit_editor_appearance(initial, root, nullptr, nullptr);
  if (!expect(apply_black.delivered() && black && black->accent && *black->accent == 0U,
              "Apply returns an explicit black accent, not an absent one")) {
    editor::apply_editor_appearance({}, root);
    return;
  }
  expect(painted_black, "explicit black, not the teal preset, paints the list selection");

  bool reopened_black{}, preset_preview{};
  ModalAction cancel_clear([&](Fl_Window &modal) {
    auto *accent = dynamic_cast<Fl_Input *>(control(modal, "Accent #RRGGBB"));
    auto *cancel = dynamic_cast<Fl_Button *>(control(modal, "Cancel"));
    if (!accent || !cancel) {
      expect(false, "the reopened dialog owns the accent field and Cancel");
      modal.hide();
      return;
    }
    reopened_black = std::string(accent->value()) == "#000000";
    accent->value("");
    accent->do_callback();
    preset_preview = selection_rgb(*list) == Fl::get_color(fl_rgb_color(0x2C, 0x42, 0x3F));
    cancel->do_callback();
  });
  const auto cancelled = editor::edit_editor_appearance(*black, root, nullptr, nullptr);
  expect(cancel_clear.delivered() && !cancelled, "Cancel returns no appearance choice");
  expect(reopened_black, "reopening shows the durable explicit black accent");
  expect(preset_preview, "clearing the field previews the preset accent again");
  expect(selection_rgb(*list) == Fl::get_color(fl_rgb_color(24, 24, 24)),
         "Cancel restores the applied black accent on the root window");

  bool reset_empty{};
  ModalAction reset([&](Fl_Window &modal) {
    auto *reset_all = dynamic_cast<Fl_Button *>(control(modal, "Reset all colours"));
    auto *apply = dynamic_cast<Fl_Button *>(control(modal, "Apply"));
    if (!reset_all || !apply) {
      expect(false, "the reopened dialog owns Reset all colours and Apply");
      modal.hide();
      return;
    }
    reset_all->do_callback();
    auto *accent = dynamic_cast<Fl_Input *>(control(modal, "Accent #RRGGBB"));
    reset_empty = accent != nullptr && std::string(accent->value()).empty();
    apply->do_callback();
  });
  const auto reset_appearance = editor::edit_editor_appearance(*black, root, nullptr, nullptr);
  expect(reset.delivered() && reset_appearance && !reset_appearance->accent && reset_empty,
         "Reset all returns the preset accent as an absent choice");
  expect(selection_rgb(*list) == Fl::get_color(fl_rgb_color(0x2C, 0x42, 0x3F)),
         "the reset appearance previews the preset palette");
  editor::apply_editor_appearance({}, root);
}

void linux_editor_main_window_text_and_tab_labels() {
  PrivateStore store;
  editor::EditorApp app;
  app.build();
  editor::EditorAppTestAccess::window(app).show();
  Fl::check();
  expect(app.session.has_document(), "the private store opens its only profile");
  for (const auto command : {editor::EditorCommand::profile_page, editor::EditorCommand::cooldown_page,
                             editor::EditorCommand::binding_page, editor::EditorCommand::panel_page,
                             editor::EditorCommand::automation_page, editor::EditorCommand::visual_page}) {
    app.command(command);
    Fl::check();
  }
  int outputs = 0;
  std::set<std::string> wrapped_labels;
  visit(editor::EditorAppTestAccess::window(app), [&](Fl_Widget &widget) {
    auto *output = dynamic_cast<Fl_Multiline_Output *>(&widget);
    if (!output) return;
    ++outputs;
    if (expect(output->wrap() != 0, "every multiline output wraps its text") && output->label())
      wrapped_labels.insert(output->label());
  });
  expect(outputs >= 8, "the main window owns its multiline outputs");
  for (const char *label : {"Preservation / module faults", "Save / reload outcome",
                            "Daemon status and capability faults", "Visual assignment"})
    expect(wrapped_labels.count(label) == 1, "the labelled multiline output is present and wrapped");
  auto *tabs = editor::EditorAppTestAccess::tabs(app);
  int bar_bottom = tabs->y();
  while (bar_bottom < tabs->y() + tabs->h() && tabs->which(tabs->x() + 8, bar_bottom) != nullptr)
    ++bar_bottom;
  std::cerr << "tab bar bottom " << bar_bottom << " (tabs y " << tabs->y() << ")\n";
  expect(bar_bottom > tabs->y(), "the tab-bar hit test finds the first tab");
  for (int index = 0; index < tabs->children(); ++index) {
    auto *page = tabs->child(index)->as_group();
    if (!page) continue;
    visit(*page, [&](Fl_Widget &widget) {
      if (widget.label() == nullptr || *widget.label() == '\0') return;
      if ((widget.align() & FL_ALIGN_TOP) == 0 || (widget.align() & FL_ALIGN_INSIDE) != 0) return;
      int label_width = 0, label_height = 0;
      widget.measure_label(label_width, label_height);
      if (!expect(widget.y() - label_height >= bar_bottom,
                  "a tab page label is not hidden under the tab bar"))
        std::cerr << "tab page label offender: \"" << widget.label() << "\" y " << widget.y()
                  << " label height " << label_height << "\n";
    });
  }
  editor::EditorAppTestAccess::window(app).hide();
}

void linux_editor_text_views_fit_content() {
  layerc::test::ScopedEnvironment backend("FLTK_BACKEND", std::filesystem::path("x11"));
  layerc::test::require_x11();
  Fl_Double_Window window(560, 400);
  auto *text = new Fl_Multiline_Output(10, 10, 540, 340);
  auto *close = new Fl_Button(440, 360, 110, 30, "Close");
  window.resizable(text);
  window.end();
  window.show();
  Fl::check();
  text->wrap(1);
  std::string value;
  for (int line = 1; line <= 24; ++line) value += "Diagnostics line " + std::to_string(line) + "\n";
  std::string long_line;
  while (long_line.size() < 220) long_line += (long_line.empty() ? "" : " ") + std::string("word");
  value += long_line;
  text->value(value.c_str());
  editor::fit_text_view(window, *text);
  Fl::check();
  fl_font(text->textfont(), text->textsize());
  int measured_width = text->w() - Fl::box_dw(text->box()) - 6, measured_height = 0;
  fl_measure(text->value(), measured_width, measured_height, 0);
  expect(measured_height + Fl::box_dh(text->box()) <= text->h(),
         "the fitted text view shows its whole wrapped value");
  expect(close->y() >= text->y() + text->h() && close->y() + close->h() <= window.h(),
         "the close button stays below the fitted text view inside the window");
  int area_x = 0, area_y = 0, area_w = 0, area_h = 0;
  Fl::screen_work_area(area_x, area_y, area_w, area_h, window.x(), window.y());
  expect(window.h() <= area_h, "the fitted window stays inside the work area");
  window.hide();
}

void linux_editor_empty_automation_simulator_ready() {
  PrivateStore store;
  editor::EditorApp app;
  app.build();
  editor::EditorAppTestAccess::window(app).show();
  Fl::check();
  expect(app.session.has_document(), "the private store opens its only profile");
  app.command(editor::EditorCommand::automation_page);
  Fl::check();
  const std::string status = editor::EditorAppTestAccess::status(app)->value();
  if (!expect(status.find("Simulator unavailable") == std::string::npos,
              "an empty automation module still resets the simulator"))
    std::cerr << "status: " << status << "\n";
  auto *json = dynamic_cast<Fl_Multiline_Input *>(control(editor::EditorAppTestAccess::window(app), "Advanced automation JSON"));
  expect(json != nullptr, "the automation page owns the JSON editor");
  if (json) {
    const std::string value = json->value();
    expect(value.find("\"profileId\": \"" + app.session.document().profile().id + "\"") !=
               std::string::npos,
           "the empty-module template names the base profile");
  }
  auto *state = dynamic_cast<Fl_Multiline_Output *>(control(editor::EditorAppTestAccess::window(app), "Simulator state"));
  expect(state != nullptr && std::string(state->value()).starts_with("Events: 0/1"),
         "a fresh profile simulator keeps its single starter root");
  expect(!app.session.document().automation(),
         "opening the page does not invent an automation module");
  expect(!app.session.document().dirty(), "opening the automation page leaves the document clean");
  editor::EditorAppTestAccess::window(app).hide();
}

void linux_editor_empty_template_apply_is_not_an_edit() {
  PrivateStore store;
  editor::EditorApp app;
  app.build();
  editor::EditorAppTestAccess::window(app).show();
  Fl::check();
  expect(app.session.has_document(), "the private store opens its only profile");
  app.command(editor::EditorCommand::automation_page);
  Fl::check();
  const bool undo_before = app.session.document().history().can_undo();
  auto *apply = control(editor::EditorAppTestAccess::window(app), "Validate / Apply JSON");
  expect(apply != nullptr, "the automation page owns the JSON apply button");
  if (apply) apply->do_callback();
  Fl::check();
  expect(!app.session.document().automation(),
         "applying the untouched empty template creates no automation module");
  expect(!app.session.document().dirty(),
         "applying the untouched empty template leaves the document clean");
  expect(app.session.document().history().can_undo() == undo_before,
         "applying the untouched empty template adds no undo step");
  const std::string status = editor::EditorAppTestAccess::status(app)->value();
  expect(status.find("Invalid automation JSON") == std::string::npos &&
             status.find("Simulator unavailable") == std::string::npos,
         "applying the untouched empty template reports neither invalid JSON nor an unavailable simulator");
  editor::EditorAppTestAccess::window(app).hide();
}
