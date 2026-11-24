// Structural regression evidence for the two completion-audit repairs:
// the calibration form's full-precision layout and a black accent that is
// distinct from the preset. Rendered judgement stays with the lead; these
// cases only assert the widget contract on a private X11 display.
#include "accessible_widgets.hpp"
#include "appearance_window.hpp"
#include "calibration_window.hpp"
#include "diagnostics_window.hpp"
#include "editor_app.hpp"
#include "editor_color.hpp"
#include "editor_menus.hpp"
#include "editor_style.hpp"
#include "profile_edit.hpp"
#include "profile_mutations.hpp"
#include "linux_editor_test_access.hpp"
#include "options_window.hpp"
#include "editor_test_environment.hpp"
#include "layerc/core/profile.hpp"
#include "layerc/platform/profile_store.hpp"

#ifdef _WIN32
#include "layerc/platform/native_theme.hpp"
#endif


#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
      : temp_("layerc-editor-visual-contract", layerc::test::EditorTestScope::widget,
              "fixture control namespace is private") {
    auto profile = editor::make_new_profile();
    profile.game.executable_name = "fixture.exe";
    expect(platform::write_profile_file(platform::profile_directory() / "fixture.json",
                                        core::serialize_profile_json(profile)).ok(),
           "store only a private fixture");
  }

private:
  layerc::test::EditorTestEnvironment temp_;
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

class ClipboardText final : public Fl_Widget {
public:
  ClipboardText() : Fl_Widget(0, 0, 0, 0) {}
  void draw() override {}
  int handle(int event) override {
    if (event != FL_PASTE) return Fl_Widget::handle(event);
    text.assign(Fl::event_text(), static_cast<std::size_t>(Fl::event_length()));
    received = true;
    return 1;
  }
  std::string text;
  bool received{};
};

std::string read_clipboard() {
  auto *group = Fl_Group::current();
  Fl_Group::current(nullptr);
  ClipboardText target;
  Fl_Group::current(group);
  Fl::paste(target, 1);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!target.received && std::chrono::steady_clock::now() < deadline) Fl::wait(.01);
  expect(target.received, "FLTK delivered the clipboard paste");
  return target.text;
}

[[nodiscard]] unsigned selection_rgb(const Fl_Widget &widget) {
  return Fl::get_color(widget.selection_color());
}

} // namespace

// Native sampling dialog on Windows: layerc_windows_fltk_calibration_tests.
#ifndef _WIN32
void linux_editor_calibration_layout_contracts() {
  layerc::test::ScopedEnvironment backend("FLTK_BACKEND", std::filesystem::path("x11"));
  layerc::test::require_editor_window();
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
#endif

void linux_editor_black_accent_dialog_contracts() {
  layerc::test::ScopedEnvironment backend("FLTK_BACKEND", std::filesystem::path("x11"));
  layerc::test::require_editor_window();
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
    auto *reset_all = dynamic_cast<Fl_Button *>(control(modal, "Reset all colors"));
    auto *apply = dynamic_cast<Fl_Button *>(control(modal, "Apply"));
    if (!reset_all || !apply) {
      expect(false, "the reopened dialog owns Reset all colors and Apply");
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

// DesignSystem.md, Roles: Appearance names the first role below a floor with
// the warning glyph and Apply refuses until the colours are readable again.
void linux_editor_appearance_contrast_floor_dialog() {
  layerc::test::ScopedEnvironment backend("FLTK_BACKEND", std::filesystem::path("x11"));
  layerc::test::require_editor_window();
  Fl_Window root(400, 300);
  root.end();
  const editor::EditorAppearance initial;
  if (editor::platform_contrast_palette()) {
    std::cout << "Not applicable: a desktop contrast theme sets the overrides aside\n";
    return;
  }
  constexpr auto list_role = static_cast<int>(editor::EditorRole::list);
  const std::string refusal = "Lists: Text / Fill contrast is 1.1:1; at least 4.5:1 is required.";
  struct Dialog {
    Fl_Choice *role{};
    Fl_Input *text{};
    Fl_Button *apply{}, *reset_role{};
    editor::EditorStatusMark *mark{};
  };
  const auto find = [](Fl_Window &modal) {
    Dialog dialog{dynamic_cast<Fl_Choice *>(control(modal, "Role")),
                  dynamic_cast<Fl_Input *>(control(modal, "Role text")),
                  dynamic_cast<Fl_Button *>(control(modal, "Apply")),
                  dynamic_cast<Fl_Button *>(control(modal, "Reset role colors")), nullptr};
    visit(modal, [&](Fl_Widget &widget) {
      if (auto *mark = dynamic_cast<editor::EditorStatusMark *>(&widget)) dialog.mark = mark;
    });
    return dialog;
  };
  // The status line follows its mark: the box the colour fields are
  // described with.
  const auto status_label = [&find](Fl_Window &modal) {
    const auto dialog = find(modal);
    if (!dialog.mark) return std::string("<no mark>");
    const int next = modal.find(dialog.mark) + 1;
    const auto *line = next < modal.children() ? modal.child(next) : nullptr;
    return line && line->label() ? std::string(line->label()) : std::string{};
  };

  bool warned{}, refused{}, cleared{};
  ModalAction fix([&](Fl_Window &modal) {
    const auto dialog = find(modal);
    if (!dialog.role || !dialog.text || !dialog.apply || !dialog.mark) {
      expect(false, "the appearance dialog owns Role, Role text, Apply and the warning mark");
      modal.hide();
      return;
    }
    expect(dialog.mark->kind() == editor::StatusKind::none && status_label(modal).empty(),
           "the preset starts without a contrast line");
    dialog.role->value(list_role);
    dialog.role->do_callback();
    dialog.text->value("#000000");
    dialog.text->do_callback();
    warned = status_label(modal) == refusal &&
             dialog.mark->kind() == editor::StatusKind::warning;
    dialog.role->value(0);
    dialog.role->do_callback();
    dialog.apply->do_callback();
    refused = modal.shown() && dialog.role->value() == list_role &&
              Fl::focus() == dialog.text &&
              std::string(dialog.text->value()) == "#000000" &&
              status_label(modal) == refusal;
    dialog.text->value("#FFFFFF");
    dialog.text->do_callback();
    cleared = status_label(modal).empty() && dialog.mark->kind() == editor::StatusKind::none;
    dialog.apply->do_callback();
    if (modal.shown()) modal.hide();
  });
  const auto fixed = editor::edit_editor_appearance(initial, root, nullptr, nullptr);
  expect(fix.delivered() && warned,
         "black list text shows the refusal's words with the warning glyph");
  expect(refused, "Apply refuses, selects the failing role and focuses its text");
  expect(cleared, "readable list text clears the line and the glyph");
  expect(fixed && fixed->overrides[static_cast<std::size_t>(list_role)].text == 0xFFFFFFU,
         "Apply succeeds once the role clears its floors");

  bool reset_refused{}, reset_cleared{};
  ModalAction reset([&](Fl_Window &modal) {
    const auto dialog = find(modal);
    if (!dialog.role || !dialog.text || !dialog.apply || !dialog.reset_role || !dialog.mark) {
      expect(false, "the appearance dialog owns Reset role colors");
      modal.hide();
      return;
    }
    dialog.role->value(list_role);
    dialog.role->do_callback();
    dialog.text->value("#000000");
    dialog.text->do_callback();
    dialog.apply->do_callback();
    reset_refused = modal.shown() && status_label(modal) == refusal;
    dialog.reset_role->do_callback();
    reset_cleared = status_label(modal).empty() &&
                    dialog.mark->kind() == editor::StatusKind::none &&
                    std::string(dialog.text->value()).empty();
    dialog.apply->do_callback();
    if (modal.shown()) modal.hide();
  });
  const auto restored = editor::edit_editor_appearance(initial, root, nullptr, nullptr);
  expect(reset.delivered() && reset_refused, "a second unreadable draft refuses again");
  expect(reset_cleared, "Reset role colors clears the line and the glyph");
  expect(restored && *restored == initial, "Apply after Reset role colors returns the preset");
  editor::apply_editor_appearance({}, root);
}

// review layer55 F-01: Appearance measures what FLTK paints in the real main
// window, a role's text on the surface beneath it, and refuses like the
// role's own pairs.
void linux_editor_appearance_painted_pairs_dialog() {
  if (editor::platform_contrast_palette()) {
    std::cout << "Not applicable: a desktop contrast theme sets the overrides aside\n";
    return;
  }
  using editor::EditorRole;
  using editor::PaintedTextPair;
  using editor::SurfacePart;
  using editor::TextInk;
  PrivateStore store;
  editor::EditorApp app;
  app.build();
  auto &window = editor::EditorAppTestAccess::window(app);
  window.show();
  Fl::check();
  for (const auto command : {editor::EditorCommand::profile_page, editor::EditorCommand::cooldown_page,
                             editor::EditorCommand::binding_page, editor::EditorCommand::panel_page,
                             editor::EditorCommand::automation_page, editor::EditorCommand::visual_page}) {
    app.command(command);
    Fl::check();
  }
  auto *profiles = editor::EditorAppTestAccess::profiles(app);
  const Fl_Widget *preview = editor::EditorAppTestAccess::preview(app);

  const auto pairs = editor::painted_text_pairs(window, profiles, preview, true);
  const auto has = [&pairs](const PaintedTextPair &pair) {
    return std::find(pairs.begin(), pairs.end(), pair) != pairs.end();
  };
  std::map<std::string, int> counts;
  for (const auto &pair : pairs) {
    std::string key(editor::role_label(pair.text));
    key += pair.ink == TextInk::verbose ? " (verbose) -> " : " -> ";
    key += editor::role_label(pair.ground.role);
    key += pair.ground.input_field ? " field" : "";
    key += pair.ground.part == SurfacePart::selection ? " selection" : " fill";
    ++counts[key];
  }
  for (const auto &[key, count] : counts) std::cerr << "painted pair " << key << "\n";
  expect(has({EditorRole::label, TextInk::role_text, {EditorRole::window, false, SurfacePart::fill}}),
         "Labels text is painted on the window fill");
  expect(has({EditorRole::checkbox, TextInk::role_text,
              {EditorRole::window, false, SurfacePart::fill}}),
         "check box text is painted on the window fill beneath it");
  expect(std::any_of(pairs.begin(), pairs.end(),
                     [](const PaintedTextPair &pair) {
                       return pair.text == EditorRole::edit && pair.ground.role == EditorRole::edit &&
                              !pair.ground.input_field;
                     }),
         "a choice's text is painted on its non-field fill");
  expect(has({EditorRole::label, TextInk::role_text,
              {EditorRole::window, false, SurfacePart::selection}}) &&
             has({EditorRole::label, TextInk::role_text,
                  {EditorRole::button, false, SurfacePart::fill}}),
         "tab labels are painted on the selected and the unselected tab");
  expect(std::none_of(pairs.begin(), pairs.end(),
                      [](const PaintedTextPair &pair) {
                        return (pair.text == EditorRole::list || pair.text == EditorRole::sidebar) &&
                               pair.ground.role != pair.text;
                      }),
         "a browser paints its own fill: FLTK draws one without a box as a down box");

  constexpr auto labels = static_cast<int>(EditorRole::label);
  constexpr auto lists = static_cast<int>(EditorRole::list);
  const std::string labels_refusal =
      "Labels: Text / Fill contrast is " +
      editor::contrast_ratio_label(editor::rgb_contrast_ratio(0x000000, 0x1F1F1F)) +
      " on the Window fill; at least 4.5:1 is required.";
  const std::string lists_refusal =
      "Lists: Text / Selection contrast is 1.0:1; at least 4.5:1 is required.";
  const auto status_line = [](Fl_Window &modal) {
    for (int index = 0; index < modal.children(); ++index)
      if (auto *mark = dynamic_cast<editor::EditorStatusMark *>(modal.child(index))) {
        const auto *line = index + 1 < modal.children() ? modal.child(index + 1) : nullptr;
        return std::pair<std::string, editor::StatusKind>{
            line && line->label() ? line->label() : "", mark->kind()};
      }
    return std::pair<std::string, editor::StatusKind>{"<no mark>", editor::StatusKind::none};
  };
  bool warned{}, described{}, refused{}, selection_warned{}, selection_refused{}, repaired{};
  ModalAction drive([&](Fl_Window &modal) {
    auto *role = dynamic_cast<Fl_Choice *>(control(modal, "Role"));
    auto *fill = dynamic_cast<Fl_Input *>(control(modal, "Role fill"));
    auto *text = dynamic_cast<Fl_Input *>(control(modal, "Role text"));
    auto *selection = dynamic_cast<Fl_Input *>(control(modal, "Role selection"));
    auto *apply = dynamic_cast<Fl_Button *>(control(modal, "Apply"));
    if (!role || !fill || !text || !selection || !apply) {
      expect(false, "the appearance dialog owns Role, its colour fields and Apply");
      modal.hide();
      return;
    }
    expect(status_line(modal).first.empty(), "the Copper preset paints readable pairs");
    // Readable on the Labels fill, painted on the Copper window.
    role->value(labels);
    role->do_callback();
    fill->value("#FFFFFF");
    fill->do_callback();
    text->value("#000000");
    text->do_callback();
    warned = status_line(modal) ==
             std::pair<std::string, editor::StatusKind>{labels_refusal, editor::StatusKind::warning};
    described = editor::accessibility::accessible_description(*text) == labels_refusal;
    role->value(0);
    role->do_callback();
    apply->do_callback();
    refused = modal.shown() && role->value() == labels && Fl::focus() == text &&
              status_line(modal).first == labels_refusal;
    fill->value("");
    fill->do_callback();
    text->value("#FFFFFF");
    text->do_callback();
    // A list selection under the list text.
    role->value(lists);
    role->do_callback();
    selection->value("#F0F0F0");
    selection->do_callback();
    selection_warned = status_line(modal) == std::pair<std::string, editor::StatusKind>{
                                                 lists_refusal, editor::StatusKind::warning};
    apply->do_callback();
    selection_refused = modal.shown() && role->value() == lists && Fl::focus() == text &&
                        status_line(modal).first == lists_refusal;
    selection->value("");
    selection->do_callback();
    repaired = status_line(modal) ==
               std::pair<std::string, editor::StatusKind>{"", editor::StatusKind::none};
    apply->do_callback();
    if (modal.shown()) modal.hide();
  });
  const auto applied = editor::edit_editor_appearance({}, window, profiles, preview);
  expect(drive.delivered() && warned,
         "black Labels text on the window shows the refusal naming the Window fill");
  expect(described, "the role text field is described by the warning line");
  expect(refused, "Apply refuses, selects Labels and focuses its text");
  expect(selection_warned && selection_refused,
         "a list selection under the list text warns and refuses on Text / Selection");
  expect(repaired, "readable colours clear the line and the glyph");
  const auto &label_override = applied ? applied->overrides[static_cast<std::size_t>(labels)]
                                       : editor::RoleOverride{};
  expect(applied && label_override.text == 0xFFFFFFU && !label_override.fill &&
             applied->overrides[static_cast<std::size_t>(lists)] == editor::RoleOverride{},
         "Apply succeeds once every painted pair clears its floor");
  editor::apply_editor_appearance({}, window, profiles, preview);
  window.hide();
}

void linux_editor_fixed_painters_in_real_tree() {
  using namespace layerc::editor;
  PrivateStore store;
  EditorApp app;
  app.build();
  auto &window = EditorAppTestAccess::window(app);
  auto *profiles = EditorAppTestAccess::profiles(app);
  auto *cooldowns = EditorAppTestAccess::cooldowns(app);
  auto *preview = EditorAppTestAccess::preview(app);
  auto *reason = EditorAppTestAccess::remove_cooldown_reason(app);
  const auto contract = [](EditorRole role, FixedInk ink, PaintedPurpose purpose, bool dimmed = false) {
    return PaintedTextPair{role, TextInk::role_text, {role}, ink, purpose, dimmed};
  };
  const auto reason_pair =
      contract(EditorRole::window, FixedInk::subtext, PaintedPurpose::disabled_reason);
  const auto tree_contract = [&] {
    const auto pairs = painted_text_pairs(window, profiles, preview, true);
    std::vector<PaintedTextPair> expected{reason_pair};
    for (const auto role : {EditorRole::sidebar, EditorRole::list})
      for (const bool dimmed : {false, true}) {
        expected.push_back(contract(role, FixedInk::secondary, PaintedPurpose::empty_list_title, dimmed));
        expected.push_back(contract(role, FixedInk::subtext, PaintedPurpose::empty_list_hint, dimmed));
      }
    std::vector<PaintedTextPair> fixed;
    std::copy_if(pairs.begin(), pairs.end(), std::back_inserter(fixed),
                 [](const auto &pair) { return pair.fixed.has_value(); });
    expect(std::is_permutation(fixed.begin(), fixed.end(), expected.begin(), expected.end()),
           "the real tree collects both named empty inks and both variants, plus the distinct reason");
    return pairs;
  };
  expect(profiles->size() > 0 && cooldowns->size() > 0 && !cooldowns->visible_r() &&
             reason->label() && *reason->label(),
         "finding painters does not depend on an empty browser or a visible tab");
  (void)tree_contract();
  expect(painted_text_pairs(*reason, nullptr, nullptr, true) == std::vector{reason_pair},
         "a labelled reason contributes its fixed ink only, not a generic Labels pair");
  const PaintedTextPair caption{EditorRole::label, TextInk::role_text, {EditorRole::window}};
  for (Fl_Widget *browser : {static_cast<Fl_Widget *>(profiles), static_cast<Fl_Widget *>(cooldowns)}) {
    const auto own = painted_text_pairs(*browser, profiles, preview, true);
    expect(browser->label() && *browser->label() &&
               std::find(own.begin(), own.end(), caption) != own.end(),
           "a painting browser keeps its caption's Labels text on the ground beneath it");
  }

  app.session.close_document();
  app.refresh();
  profiles->clear();
  expect(cooldowns->size() == 0 && profiles->size() == 0, "both browser roles are empty");
  reason->label("");
  expect(!reason->visible() &&
             painted_text_pairs(*reason, nullptr, nullptr, true) == std::vector{reason_pair},
         "a hidden reason with an empty label still contributes its declaration");
  EditorAppearance copper;
  copper.overrides[static_cast<std::size_t>(EditorRole::sidebar)].fill = 0x444444;
  const auto failure = first_role_contrast_failure(copper, SystemRoleColors{}, tree_contract());
  expect(failure && failure->purpose == PaintedPurpose::empty_list_hint &&
             failure->ground == EditorRole::sidebar && !failure->dimmed,
         "the contrast model finds the sidebar hint refusal from pairs collected in the real tree");

  OptionsWindow options({}, {}, 1000, {}, {}, {});
  std::vector<PaintedTextPair> pinned;
  for (const auto &text : options_painted_text) {
    pinned.push_back(contract(EditorRole::window, text.ink, text.purpose));
    expect(text.ground == PaintedGround::beneath && !text.dimmed_variant,
           "Options declares only fixed, active text on the ground beneath it");
  }
  auto options_pairs = painted_text_pairs(options, nullptr, nullptr, true);
  std::erase_if(options_pairs, [](const auto &pair) { return !pair.fixed; });
  expect(options_pairs == pinned, "the real Options window and its pinned fixed pairs agree");
  int sources{};
  visit(options, [&](Fl_Widget &widget) {
    if (dynamic_cast<PaintedTextSource *>(&widget)) {
      ++sources;
      expect(painted_text_pairs(widget, nullptr, nullptr, true) == pinned,
             "an Options hint contributes no generic Labels pair");
    }
  });
  expect(sources > 0, "Options contains real hint painters");
}

void linux_editor_fixed_ink_appearance_dialog() {
#ifdef _WIN32
  const struct ContrastOverride {
    ~ContrastOverride() { layerc::platform::set_native_high_contrast_override(std::nullopt); }
  } reset;
  layerc::platform::set_native_high_contrast_override(false);
#else
  if (editor::platform_contrast_palette()) {
    std::cout << "Not applicable: a desktop contrast theme sets the overrides aside\n";
    return;
  }
#endif
  using namespace layerc::editor;
  layerc::test::EditorTestEnvironment store("layerc-editor-fixed-ink", layerc::test::EditorTestScope::widget,
                                            "fixture control namespace is private");
  EditorApp app;
  app.build();
  auto &window = EditorAppTestAccess::window(app);
  auto *profiles = EditorAppTestAccess::profiles(app);
  auto *preview = EditorAppTestAccess::preview(app);
  window.show();
  Fl::check();
  const EditorAppearance copper;
  expect(profiles->size() == 0, "the private Copper Profiles list starts empty");
  constexpr auto sidebar = static_cast<int>(EditorRole::sidebar);
  const std::string refusal = "Profile list: empty-list hint contrast is " +
      contrast_ratio_label(rgb_contrast_ratio(editor_tokens(copper).subtext, 0x444444)) +
      " on the fill; at least 4.5:1 is required.";
  ModalAction drive([&](Fl_Window &modal) {
    auto *role = dynamic_cast<Fl_Choice *>(control(modal, "Role"));
    auto *fill = dynamic_cast<Fl_Input *>(control(modal, "Role fill"));
    auto *text = dynamic_cast<Fl_Input *>(control(modal, "Role text"));
    auto *apply = control(modal, "Apply");
    Fl_Widget *status{};
    for (int i = 0; i + 1 < modal.children(); ++i)
      if (dynamic_cast<EditorStatusMark *>(modal.child(i))) status = modal.child(i + 1);
    if (!expect(role && fill && text && apply && status, "Appearance owns its fields and warning line")) {
      modal.hide();
      return;
    }
    expect(status->w() == 472 && status->h() == 50, "the warning line keeps its 472 x 50 row");
    fl_font(status->labelfont(), FL_NORMAL_SIZE);
    for (std::size_t i = 0; i < editor_role_count; ++i) {
      const auto ground = static_cast<EditorRole>(i);
      for (const auto purpose : {PaintedPurpose::empty_list_title, PaintedPurpose::empty_list_hint,
                                 PaintedPurpose::disabled_reason, PaintedPurpose::options_hint})
        for (const bool dimmed : {false, true}) {
          const auto message = role_contrast_failure_text(
              {ground, ContrastPairKind::text_fill, 4.499, ground, SurfacePart::fill, false, purpose, dimmed});
          int width = status->w(), height = 0;
          fl_measure(message.c_str(), width, height, 0);
          expect(width <= status->w() && height <= status->h(), "the generated refusal fits: " + message);
        }
    }

    role->value(sidebar);
    role->do_callback();
    text->value("#FFFFFF");
    text->do_callback();
    fill->value("#444444");
    fill->do_callback();
    expect(status->label() && refusal == status->label() &&
               accessibility::accessible_description(*fill) == refusal &&
               accessibility::accessible_description(*text) == refusal,
           "the live warning and field descriptions use the exact fixed-ink refusal");
    role->value(static_cast<int>(EditorRole::window));
    role->do_callback();
    apply->do_callback();
    const bool still_open = modal.shown();
    expect(still_open && role->value() == sidebar && Fl::focus() == fill &&
               status->label() && refusal == status->label(),
           "Apply refuses and focuses Profile list Fill even with an explicit passing Text override");
    if (!still_open) return;
    fill->value("#171717");
    fill->do_callback();
    expect((!status->label() || !*status->label()) && accessibility::accessible_description(*fill).empty(),
           "repairing the fill clears the warning and its accessible description");
    apply->do_callback();
    if (modal.shown()) modal.hide();
  });
  const auto result = edit_editor_appearance(copper, window, profiles, preview);
  expect(drive.delivered() && result && result->overrides[sidebar].fill == 0x171717 &&
             result->overrides[sidebar].text == 0xFFFFFF,
         "repaired Copper draft applies with the Text override intact");
  apply_editor_appearance(copper, window, profiles, preview);
  window.hide();
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
  expect(outputs >= (editor::windows_editor_keys ? 7 : 8),
         "the main window owns its multiline outputs");
  if constexpr (editor::windows_editor_keys) {
    auto *id = dynamic_cast<Fl_Input *>(control(editor::EditorAppTestAccess::window(app), "Profile ID"));
    expect(id && !id->readonly(), "Windows replaces the Profile ID output with an editable field");
  } else {
    expect(wrapped_labels.count("Profile ID (read-only)") == 1,
           "Linux retains the wrapped Profile ID output");
  }
  for (const char *label : {"Preservation / module faults", "Save / reload outcome",
                            "LayerD status and capability faults", "Visual assignment"})
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
  layerc::test::require_editor_window();
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

void save_as_shortcut_saves_separate_named_profile() {
  PrivateStore store;
  editor::EditorApp app;
  app.build();
  app.main_window()->show();
  if (!expect(app.session.has_document(), "the starting profile is open")) return;
  const auto original = app.session.document().profile();
  const auto path = app.session.document().path();
  const auto bytes = layerc::test::read_file(path);
  ModalAction answer([](Fl_Window &modal) {
    Fl_Input *input = nullptr;
    visit(modal, [&](Fl_Widget &widget) {
      if (!input && widget.visible_r()) input = dynamic_cast<Fl_Input *>(&widget);
    });
    auto *ok = control(modal, "OK");
    if (!expect(input && ok && modal.label() && std::string_view(modal.label()) == "Save profile as",
                "Save As opens its name prompt")) {
      modal.hide();
      return;
    }
    input->value("Saved copy");
    ok->do_callback();
  });
  expect(editor::EditorAppTestAccess::menus(app)->shortcut('s', FL_CTRL | FL_SHIFT) &&
             answer.delivered(),
         "Ctrl+Shift+S invokes the actual Save As prompt");
  const auto &copy = app.session.document();
  expect(!app.session.is_draft() && !copy.dirty() && copy.path() != path &&
             copy.profile().id != original.id && copy.profile().name == "Saved copy" &&
             copy.profile().cooldowns == original.cooldowns &&
             layerc::test::read_file(copy.path()).value_or("").find("Saved copy") != std::string::npos,
         "the copy has a separate identity and saved file with the chosen name");
  expect(bytes.has_value() && layerc::test::read_file(path) == bytes,
         "Save As does not overwrite the original");
}

void duplicate_cooldown_menu_context_and_shortcut() {
  PrivateStore store;
  editor::EditorApp app;
  app.build();
  app.main_window()->show();
  if (!expect(app.session.has_document(), "the starting profile is open")) return;
  app.command(editor::EditorCommand::cooldown_page);
  const auto original = app.session.document().profile();
  const auto path = app.session.document().path();
  auto *menus = editor::EditorAppTestAccess::menus(app);
  auto &bar = menus->bar();
  Fl_Menu_Button context(0, 0, 0, 0);
  menus->configure_context(context, editor::EditorMenuContext::cooldowns, true);
  const auto *item = bar.find_item("&Cooldowns/D&uplicate cooldown");
  const auto *profile_item = bar.find_item("&Profile/&Duplicate");
  const auto *context_item = context.find_item("D&uplicate cooldown");
  if (!expect(item && item->active() && profile_item && context_item && context_item->active(),
              "the menu bar and cooldown context offer their correct commands")) return;
  const int key = FL_CTRL | 'd';
  const int cooldown_key = editor::windows_editor_keys ? key : 0;
  expect(item->shortcut() == cooldown_key && context_item->shortcut() == cooldown_key &&
             profile_item->shortcut() == (editor::windows_editor_keys ? 0 : key),
         "menus advertise the same platform-specific Ctrl+D assignment");
  const auto verify_duplicate = [&] {
    const auto &profile = app.session.document().profile();
    if (expect(profile.id == original.id && app.session.document().path() == path &&
                   !app.session.is_draft() && profile.cooldowns.size() == original.cooldowns.size() + 1 &&
                   profile.bindings == original.bindings && app.session.can_undo(),
               "duplication adds a cooldown without new bindings, in the same document"))
      expect(profile.cooldowns.back().id != original.cooldowns.front().id &&
                 profile.cooldowns.back().name == original.cooldowns.front().name + " copy",
             "the selected cooldown is copied under its own identity");
    app.command(editor::EditorCommand::undo);
    expect(app.session.document().profile() == original && !app.session.can_undo(),
           "one Undo removes the copy with no extra edit");
  };
  const auto bar_action = *item;
  const auto context_action = *context_item;
  bar_action.do_callback(&bar);
  verify_duplicate();
  context_action.do_callback(&context);
  verify_duplicate();
  expect(menus->shortcut('d', FL_CTRL), "Ctrl+D is handled by the command registry");
  if constexpr (editor::windows_editor_keys) {
    verify_duplicate();
  } else {
    expect(app.session.is_draft() && app.session.document().profile().id != original.id &&
               app.session.document().profile().cooldowns == original.cooldowns,
           "Linux keeps Ctrl+D as duplicate profile, not duplicate cooldown");
  }
}

void hide_when_ready_updates_panel_and_keeps_edit_preview_visible() {
  PrivateStore store;
  editor::EditorApp app;
  app.build();
  app.main_window()->show();
  app.command(editor::EditorCommand::panel_page);
  if (!expect(app.session.has_document(), "the starting profile is open")) return;
  const auto original = app.session.document().profile().panels.front();
  auto *check = dynamic_cast<Fl_Check_Button *>(control(*app.main_window(), "Hide bars when ready"));
  if (!expect(check && check->active_r(), "the selected panel offers the Hide bars when ready control"))
    return;
  check->value(1);
  check->do_callback();
  const auto &panel = app.session.document().profile().panels.front();
  const auto preview = editor::EditorAppTestAccess::preview_panel(app, panel);
  expect(panel.bar.hide_when_ready && preview.panel.bar.hide_when_ready &&
             preview.presentation.visible && !preview.presentation.items.empty(),
         "the option reaches the preview, which still shows ready bars in edit mode");
  app.command(editor::EditorCommand::undo);
  expect(app.session.document().profile().panels.front() == original && !app.session.can_undo(),
         "one Undo restores the original panel");
}

void split_grouped_bars_creates_independent_panels_in_one_edit() {
  PrivateStore store;
  editor::EditorApp app;
  app.build();
  app.main_window()->show();
  if (!expect(app.session.has_document(), "the starting profile is open")) return;
  expect(!app.command_enabled(editor::EditorCommand::split_bars), "an ungrouped bar cannot be split");
  const auto original = app.session.document().profile();
  if (!expect(!app.session.edit([](auto &profile) {
        auto second = profile.cooldowns.front();
        second.id = "other";
        second.name = "Other ability";
        profile.cooldowns.push_back(std::move(second));
        profile.panels.front().cooldown_ids.emplace_back("other");
      }), "seed one bar with two cooldowns")) return;
  app.refresh();
  const auto grouped = app.session.document().profile();
  expect(app.command_enabled(editor::EditorCommand::split_bars), "a grouped bar enables Split grouped bars");
  app.command(editor::EditorCommand::split_bars);
  const auto &panels = app.session.document().profile().panels;
  const std::string_view status = editor::EditorAppTestAccess::status(app)->value();
  expect(panels.size() == 2 &&
             panels[0].cooldown_ids == std::vector{grouped.cooldowns[0].id} &&
             panels[1].cooldown_ids == std::vector{grouped.cooldowns[1].id} &&
             panels[0].id != panels[1].id && panels[0].anchor_y < panels[1].anchor_y &&
             status.starts_with("Split grouped bars into independently positioned cooldown bars."),
         "Split grouped bars separates cooldowns, positions their panels and reports success");
  app.command(editor::EditorCommand::undo);
  expect(app.session.document().profile() == grouped,
         "one Undo restores the complete grouped panel");
  app.command(editor::EditorCommand::undo);
  expect(app.session.document().profile() == original && !app.session.can_undo(),
         "only the earlier seed edit precedes the split");
  app.session.close_document();
  app.refresh();
  expect(!app.command_enabled(editor::EditorCommand::split_bars), "no document disables Split grouped bars");
}

void runtime_diagnostics_copies_unformatted_json_and_closes() {
  PrivateStore store;
  editor::DaemonStatus status;
  status.payload_json = R"({"fixture":"clipboard"})";
  editor::DiagnosticsWindow diagnostics([&] { return status; }, [] {
    return editor::DaemonCommandResult{};
  });
  diagnostics.show();
  auto *window = Fl::first_window();
  if (!expect(window && window->label() &&
                  std::string_view(window->label()) == "Runtime diagnostics — LayerE",
              "the diagnostics window is shown")) return;
  auto *copy = control(*window, "&Copy raw JSON");
  auto *close = control(*window, "Close");
  if (!expect(copy && copy->active_r() && close, "the snapshot enables Copy and Close")) return;
  copy->do_callback();
  expect(read_clipboard() == status.payload_json, "Copy raw JSON pastes the original snapshot bytes");
  close->do_callback();
  expect(!diagnostics.shown(), "the Close button hides runtime diagnostics");
}

void about_shows_product_and_copies_build_details() {
  PrivateStore store;
  editor::EditorApp app;
  app.build();
  app.main_window()->show();
  ModalAction verify([](Fl_Window &modal) {
    auto *body = dynamic_cast<Fl_Multiline_Output *>(control(modal, "About LayerE"));
    auto *copy = control(modal, "&Copy details");
    auto *close = control(modal, "Close");
    if (expect(body && copy && close && modal.label() &&
                   std::string_view(modal.label()) == "About LayerE",
               "Help > About displays its text and copy action")) {
      const std::string_view text = body->value();
      expect(text.starts_with("LayerE ") && text.find("Creates and edits cooldown overlay profiles.") !=
                 std::string_view::npos, "About describes LayerE and its purpose");
#ifdef _WIN32
      constexpr std::string_view promise =
          "LayerC reads input passively and never changes or sends it. Only two editor modes "
          "that you start yourself take mouse input. Pick window captures the mouse for your "
          "next left click; pressing a key other than a modifier, losing mouse capture or the "
          "end of the edit cancels the pick. Direct drag, "
          "once you agree, lets LayerD's visible panels take the clicks that land on them while "
          "you position them on the game window. The editor never accesses the network.";
#else
      constexpr std::string_view promise =
          "LayerC reads input passively: it never blocks, changes or sends input, and the editor "
          "never accesses the network.";
#endif
      expect(text.find(promise) != std::string_view::npos,
             "About states this platform's input exceptions and the no-network promise");
      copy->do_callback();
      auto details = read_clipboard();
      expect(details.starts_with("LayerE ") &&
                 details.find(" build), FLTK " + std::to_string(Fl::api_version())) != std::string::npos &&
                 details.find(", display scale ") != std::string::npos &&
                 details != text,
             "Copy details pastes version, build, toolkit and scale rather than the About body");
    }
    if (close) close->do_callback();
    else modal.hide();
  });
  app.command(editor::EditorCommand::about);
  expect(verify.delivered(), "the About modal was shown and dismissed");
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
