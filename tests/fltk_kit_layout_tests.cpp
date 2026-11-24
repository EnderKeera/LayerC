#include "a11y/fltk_tree.hpp"
#include "accessible_widgets.hpp"
#include "editor_style.hpp"
#include "editor_test_environment.hpp"
#include "kit/layout.hpp"
#include "options_window.hpp"
#include "test_support.hpp"

#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Input.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace {
namespace editor = layerc::editor;
namespace kit = editor::kit;
namespace a11y = editor::a11y;
using kit::Metric;
using layerc::test::expect;

struct BodySize {
  const int old_font = FL_NORMAL_SIZE;
  const int old_body = kit::body();
  ~BodySize() {
    FL_NORMAL_SIZE = static_cast<Fl_Fontsize>(old_font);
    kit::set_body(old_body);
  }
  static void set(const int value) {
    FL_NORMAL_SIZE = static_cast<Fl_Fontsize>(value);
    kit::set_body(value);
  }
};

struct Bounds {
  int x, y, w, h;
  bool operator==(const Bounds &) const = default;
};

Bounds bounds(const Fl_Widget &widget) { return {widget.x(), widget.y(), widget.w(), widget.h()}; }
int right(const Fl_Widget &widget) { return widget.x() + widget.w(); }
int bottom(const Fl_Widget &widget) { return widget.y() + widget.h(); }
int field_gap() { return kit::metric(Metric::gap_dialog); }
int control_height() { return kit::metric(Metric::control_height); }

void check_containment(const Fl_Group &group) {
  const int x = group.as_window() ? 0 : group.x();
  const int y = group.as_window() ? 0 : group.y();
  for (int i = 0; i < group.children(); ++i) {
    const auto &child = *group.child(i);
    if (!child.visible()) continue;
    expect(child.x() >= x && child.y() >= y && right(child) <= x + group.w() &&
               bottom(child) <= y + group.h(), "visible child fits inside its layout container");
    for (int j = i + 1; j < group.children(); ++j) {
      const auto &sibling = *group.child(j);
      if (sibling.visible())
        expect(right(child) <= sibling.x() || right(sibling) <= child.x() ||
                   bottom(child) <= sibling.y() || bottom(sibling) <= child.y(),
               "visible siblings do not intersect");
    }
    if (auto *nested = group.child(i)->as_group()) check_containment(*nested);
  }
}

void check_buttons(const kit::ButtonRow &row, const Fl_Button &ok, const Fl_Button &cancel,
                   const Fl_Button &apply) {
  const int gap = field_gap();
  expect(ok.h() == control_height() && cancel.h() == ok.h() && apply.h() == ok.h(),
         "every action has the control height");
  expect(cancel.x() - right(ok) == gap && apply.x() - right(cancel) == gap &&
             right(apply) == right(row), "OK, Cancel and Apply are ordered, spaced and right aligned");
  for (const auto *button : {&ok, &cancel, &apply})
    expect(button->w() == std::max(kit::metric(Metric::button_min_width),
                                 kit::text_width(button->label()) + kit::metric(Metric::button_text_pad)),
           "actions fit measured captions and token padding");
}

void check_frame(const Fl_Window &window, const kit::Form &form, const kit::ButtonRow &row) {
  const int margin = kit::metric(Metric::margin);
  expect(form.x() == margin && form.y() == margin && right(form) == window.w() - margin &&
             row.x() == margin && right(row) == window.w() - margin && bottom(row) == window.h() - margin,
         "frame applies all four margins");
  expect(row.y() - bottom(form) == kit::metric(Metric::gap_main), "main gap separates actions from the form");
  expect(form.h() == form.height_for(form.w()), "form extent matches its measurement");
  check_containment(window);
}

void form_contract() {
  Fl_Double_Window window(1, 1);
  auto &form = *new kit::Form;
  auto &input = *new Fl_Input(0, 0, 0, 0);
  const auto &side = form.field_row("Profile &name", input);
  auto &choice = *new editor::EditorChoice(0, 0, 0, 0);
  const auto &combo_label = form.field_row("Longest field &label", choice, 110);
  const auto &field_hint = form.hint(choice, "A hint for the menu.");
  form.gap(Metric::section_gap);
  auto &check = *new Fl_Check_Button(0, 0, 0, 0,
      "&Check a long enough label to wrap on a narrow dialog instead of losing the words about the setting");
  form.check_row(check);
  const auto &check_hint = form.hint(check, "A hint for the checkbox.");
  form.end();
  auto &row = *new kit::ButtonRow;
  auto &reset = *new Fl_Button(0, 0, 0, 0, "Reset");
  row.leading(reset);
  auto &ok = *new Fl_Button(0, 0, 0, 0, "OK");
  row.action(ok);
  auto &cancel = *new Fl_Button(0, 0, 0, 0, "Cancel");
  row.action(cancel);
  auto &apply = *new Fl_Button(0, 0, 0, 0, "&Apply selected settings");
  row.action(apply);
  row.end();
  window.end();
  constexpr int work_cap = 1100;
  kit::DialogFrame frame(window, form, row, 740, work_cap, kit::DialogSizing::resizable);

  const auto measure_layout = [&] {
    return std::array{bounds(form), bounds(side), bounds(input), bounds(combo_label), bounds(choice),
                      bounds(field_hint), bounds(check), bounds(check_hint), bounds(row), bounds(reset),
                      bounds(ok), bounds(cancel), bounds(apply)};
  };
  const auto inspect = [&](const int body) {
    const int label_column = kit::text_width(combo_label.label()) + field_gap();
    expect(form.label_column() == label_column && side.w() == label_column &&
               input.x() == form.x() + label_column && choice.x() == input.x(),
           "eager grid uses widest label plus the dialog gap for the field column");
    expect(input.h() == control_height() && input.w() == (body == 12 ? 480 : 560),
           "standard field has kit height and the scaled field cap");
    expect(choice.h() == kit::metric(Metric::combo_height) && choice.w() == kit::scaled(110) &&
               choice.y() - combo_label.y() == (control_height() - choice.h()) / 2,
           "fixed-width drop-down is vertically centred in the control row");
    expect(combo_label.y() - side.y() == kit::metric(Metric::row_pitch), "ordinary rows step by row pitch");
    expect(field_hint.x() == choice.x() && field_hint.y() == bottom(combo_label) &&
               check.y() - bottom(field_hint) == field_gap() + kit::metric(Metric::section_gap) &&
               check_hint.x() - check.x() == kit::check_label_offset() && check_hint.y() == bottom(check),
           "hints align below their control labels; the hint gap and explicit gap are additive");
    expect(editor::accessibility::accessible_description(choice) == field_hint.label() &&
               editor::accessibility::accessible_description(check) == check_hint.label(),
           "hints describe their owners");
    expect(reset.x() == row.x(), "optional leading action stays at the left edge");
    check_buttons(row, ok, cancel, apply);
    check_frame(window, form, row);
    int work_x = 0, work_y = 0, work_w = 0, work_h = 0;
    Fl::screen_work_area(work_x, work_y, work_w, work_h, window.screen_num());
    const int margin = kit::metric(Metric::margin);
    const int cap = std::max(frame.minimum_width(),
                             std::min(work_w - (window.decorated_w() - window.w()), work_cap));
    int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    expect(window.get_size_range(&min_w, &min_h, &max_w, &max_h) &&
               min_w == frame.minimum_width() &&
               min_h == 2 * margin + form.height_for(cap - 2 * margin) +
                            kit::metric(Metric::gap_main) + control_height() &&
               max_w == 0 && max_h == 0,
           "resizable frame gets its minimum extent from the form and remains unbounded");
  };
  inspect(12);
  const auto original = measure_layout();
  BodySize::set(14);
  inspect(14);
  frame.refit(frame.minimum_width());
  expect(window.w() == frame.minimum_width() && check.w() == form.w() && check.h() > control_height(),
         "minimum-width refit wraps a full-span checkbox and grows its row");
  check_frame(window, form, row);
  const int before = check.y();
  choice.hide();
  frame.refit(frame.minimum_width());
  expect(!combo_label.visible() && !field_hint.visible() && check.y() < before,
         "hiding a field collapses its row, label and hint synchronously");
  choice.show();
  frame.refit();
  BodySize::set(12);
  expect(measure_layout() == original, "body 12 → 14 → 12 restores all original geometry");
  int work_x = 0, work_y = 0, work_w = 0, work_h = 0;
  Fl::screen_work_area(work_x, work_y, work_w, work_h, window.screen_num());
  window.position(work_x + work_w - 1, work_y + work_h - 1);
  frame.place();
  const int edge = (window.decorated_w() - window.w()) / 2;
  const int title_and_edge = window.decorated_h() - window.h() - edge;
  expect(window.x() - edge >= work_x && window.y() - title_and_edge >= work_y &&
             right(window) + edge == work_x + work_w && bottom(window) + edge == work_y + work_h,
         "frame moves only as far as needed for the decorated window to fit the work area");
}

void options_contract() {
  editor::OptionsWindow options({}, {}, 335, {}, {}, {});
  auto *form = dynamic_cast<kit::Form *>(options.child(0));
  auto *row = dynamic_cast<kit::ButtonRow *>(options.child(1));
  if (!expect(form && row, "Options uses the kit grid and flex row")) return;
  const Fl_Choice *choice = nullptr;
  std::array<const Fl_Check_Button *, 4> checks{};
  std::size_t count = 0;
  for (int i = 0; i < form->children(); ++i) {
    if (const auto *menu = dynamic_cast<const Fl_Choice *>(form->child(i))) choice = menu;
    if (const auto *check = dynamic_cast<const Fl_Check_Button *>(form->child(i)); check && count < checks.size())
      checks[count++] = check;
  }
  if (!expect(choice && count == checks.size(), "Options retains its drop-down and four checkboxes")) return;
  const auto *side = editor::accessibility::accessible_label(*choice);
  const auto *ok = dynamic_cast<const Fl_Button *>(row->child(row->children() - 3));
  const auto *cancel = dynamic_cast<const Fl_Button *>(row->child(row->children() - 2));
  const auto *apply = dynamic_cast<const Fl_Button *>(row->child(row->children() - 1));
  if (!expect(side && ok && cancel && apply, "Options has its side label and its three actions")) return;
  const auto inspect = [&] {
    const int margin = kit::metric(Metric::margin);
    const int min_width = std::max(form->minimum_width(), row->width()) + 2 * margin;
    expect(options.w() == std::clamp(std::max(kit::scaled(480), form->widest_check() + 2 * margin),
                                    min_width, std::max(min_width, 335)),
           "pilot measures preferred width and clamps it to the work area");
    expect(!choice->label() && std::string_view(side->label()) == "&Text size" &&
               choice->x() == form->x() + kit::text_width(side->label()) + field_gap() &&
               choice->y() == side->y() + (side->h() - choice->h()) / 2 &&
               choice->h() == kit::metric(Metric::combo_height) && choice->w() == kit::scaled(110),
           "pilot has a side label and a centred fixed logical-width combo");
    expect(std::string_view(ok->label()) == "OK" && std::string_view(cancel->label()) == "Cancel" &&
               std::string_view(apply->label()) == "&Apply" && editor::is_primary_button(*ok) &&
               !apply->active(), "pilot preserves the action order, primary button and initial Apply state");
    const auto *overlays = form->child(form->find(checks[1]) + 1);
    expect(overlays->x() - checks[1]->x() == kit::check_label_offset() &&
               overlays->y() == bottom(*checks[1]) &&
               editor::accessibility::accessible_description(*checks[1]) == overlays->label(),
           "pilot's FLTK-only overlays hint stays indented directly below its checkbox");
    check_buttons(*row, *ok, *cancel, *apply);
    check_frame(options, *form, *row);
    int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    expect(options.get_size_range(&min_w, &min_h, &max_w, &max_h) &&
               min_w == options.w() && max_w == options.w() &&
               min_h == options.h() && max_h == options.h(),
           "Options stays fixed at its newly fitted client size");
  };
  inspect();
  const auto original = std::array{bounds(options), bounds(*choice), bounds(*apply)};
  BodySize::set(14);
  inspect();
  BodySize::set(12);
  expect(std::array{bounds(options), bounds(*choice), bounds(*apply)} == original,
         "live Options refits back to its initial geometry without rebuilding");
}

std::unique_ptr<Fl_RGB_Image> paint_form(kit::Form &form, const Fl_Window &window) {
  Fl_Image_Surface surface(window.w(), window.h());
  Fl_Surface_Device::push_current(&surface);
  fl_color(window.color());
  fl_rectf(0, 0, window.w(), window.h());
  surface.draw(&form, form.x(), form.y());
  Fl_Surface_Device::pop_current();
  return std::unique_ptr<Fl_RGB_Image>(surface.image());
}

void check_label_ink(kit::Form &form, const Fl_Window &window, Fl_Widget &label,
                     const Fl_RGB_Image &painted, const char *column_message, const char *bounds_message) {
  const auto label_type = label.labeltype();
  label.labeltype(FL_NO_LABEL);
  const auto blank = paint_form(form, window);
  label.labeltype(label_type);
  if (!expect(blank && painted.array && blank->array && painted.d() == 3 && blank->d() == 3 &&
                  painted.data_w() == window.w() && painted.data_h() == window.h() &&
                  blank->data_w() == window.w() && blank->data_h() == window.h(),
              "offscreen form exposes a full RGB image")) return;
  const int stride = painted.ld() ? painted.ld() : 3 * window.w();
  const int blank_stride = blank->ld() ? blank->ld() : 3 * window.w();
  int first = window.w();
  bool contained = true;
  for (int y = 0; y < window.h(); ++y) {
    for (int x = 0; x < window.w(); ++x) {
      const auto *ink = painted.array + y * stride + 3 * x;
      if (std::equal(ink, ink + 3, blank->array + y * blank_stride + 3 * x)) continue;
      first = std::min(first, x);
      contained &= x >= label.x() && x < right(label) && y >= label.y() && y < bottom(label);
    }
  }
  expect(first >= label.x() && first <= label.x() + 2, column_message);
  expect(contained, bounds_message);
}

void label_paint_contract() {
  Fl_Double_Window window(1, 1);
  auto &form = *new kit::Form;
  auto &input = *new Fl_Input(0, 0, 0, 0);
  auto &side = form.field_row("Name", input);
  auto &hint = form.hint(input, "");
  auto &next = *new Fl_Input(0, 0, 0, 0);
  form.field_row("Next", next);
  form.end();
  auto &row = *new kit::ButtonRow;
  row.action(*new Fl_Button(0, 0, 0, 0, "OK"));
  row.end();
  window.end();
  kit::DialogFrame frame(window, form, row, 400, 800, kit::DialogSizing::resizable);

  const int available = form.w() - form.label_column();
  std::string words = "Hint";
  while (kit::text_width(words.c_str()) <= available - 6) words += " fits";
  hint.copy_label(words.c_str());
  const int measured = kit::text_width(words.c_str());
  frame.refit(window.w() + measured + 3 - available);
  if (!expect(measured > hint.w() - 6 && measured <= hint.w() &&
                  hint.h() == kit::wrapped_height("Hint", hint.w()),
              "the near-limit field hint reserves exactly one line")) return;
  const auto painted = paint_form(form, window);
  if (!expect(painted != nullptr, "capture the kit form offscreen")) return;
  check_label_ink(form, window, side, *painted, "RowLabel ink starts at its column",
                  "RowLabel ink stays inside its bounds");
  check_label_ink(form, window, hint, *painted, "field hint ink starts at its column",
                  "near-limit field hint ink stays inside its one-line bounds");
}

int shortcut(Fl_Widget &widget, char key) {
  const auto before_key = Fl::e_keysym;
  const auto before_state = Fl::e_state;
  const auto before_text = Fl::e_text;
  const auto before_length = Fl::e_length;
  char text[]{key, '\0'};
  Fl::e_keysym = key;
  Fl::e_state = FL_ALT;
  Fl::e_text = text;
  Fl::e_length = 1;
  const auto result = widget.handle(FL_SHORTCUT);
  Fl::e_keysym = before_key;
  Fl::e_state = before_state;
  Fl::e_text = before_text;
  Fl::e_length = before_length;
  return result;
}

} // namespace

void fltk_kit_layout_geometry() {
  layerc::test::EditorTestEnvironment fixture("layerc-kit-layout", layerc::test::EditorTestScope::widget);
  editor::install_editor_style();
  const BodySize restore;
  BodySize::set(12);
  form_contract();
  options_contract();
  label_paint_contract();
}

void fltk_kit_label_accessibility() {
  layerc::test::EditorTestEnvironment fixture("layerc-kit-label", layerc::test::EditorTestScope::widget);
  editor::install_editor_style();
  editor::OptionsWindow options({}, {}, 640, {}, {}, {});
  options.show();
  Fl::check();
  auto *form = dynamic_cast<kit::Form *>(options.child(0));
  if (!expect(form, "Options exposes the real kit form")) return;
  Fl_Choice *choice = nullptr;
  for (int i = 0; i < form->children(); ++i)
    if ((choice = dynamic_cast<Fl_Choice *>(form->child(i)))) break;
  if (!expect(choice, "Options has its text-size dropdown")) return;
  auto *label = editor::accessibility::accessible_label(*choice);
  if (!expect(dynamic_cast<kit::RowLabel *>(label), "side label is tracked for the real dropdown")) return;
  expect(shortcut(*form, 'T') && Fl::focus() == choice, "Alt+T on the side label focuses the dropdown");
  choice->deactivate();
  expect(!shortcut(*form, 'T'), "the side label cannot focus an inactive dropdown");
  choice->activate();

  a11y::FltkTree tree;
  const auto snapshot = tree.snapshot();
  const auto id = tree.id_for(*choice);
  const auto label_id = tree.id_for(*label);
  if (expect(snapshot.nodes.contains(id) && snapshot.nodes.contains(label_id), "both sides export in the same snapshot")) {
    const auto &node = snapshot.nodes.at(id);
    expect(node.role == a11y::Role::drop_down && node.name == "Text size" && node.keybinding == "<Alt>t;;" &&
               node.description == "Adds to the Windows text size setting.",
           "label exports name and mnemonic without losing the dropdown's role or hint");
    expect(node.labelled_by == std::vector{label_id} && snapshot.nodes.at(label_id).name == "Text size",
           "dropdown's labelled_by relation points to the exported side label");
  }
  form->remove(*label);
  const auto without_label = tree.snapshot();
  expect(tree.id_for(*label) == a11y::no_node && without_label.nodes.at(id).labelled_by.empty(),
         "a label absent from the current snapshot is not related");
  form->insert(*label, choice);
  Fl_Input detached(0, 0, 0, 0);
  {
    Fl_Box temporary(0, 0, 0, 0, "Temporary label");
    editor::accessibility::label_with(detached, temporary);
  }
  expect(editor::accessibility::accessible_label(detached) == nullptr,
         "destroying a label clears the tracked relation");
  options.hide();
}
