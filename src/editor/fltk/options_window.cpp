#include "options_window.hpp"

#include "accessible_widgets.hpp"
#include "appearance_window.hpp"
#include "editor_app.hpp"
#include "editor_style.hpp"

#include <FL/Fl_Box.H>
#include <FL/Fl_Check_Button.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace layerc::editor {
namespace {

constexpr int margin = 12, gap = 8, min_button_width = 88;
constexpr int choice_width = 110, preferred_width = 480;
constexpr auto title = "Options — LayerE";
constexpr auto visual_label = "Enable &visual recognition";
constexpr auto overlays_label = "Hide cooldown &overlays from screen capture and streams";
constexpr auto editor_label = "Hide the &editor from screen capture";
constexpr auto text_size_label = "&Text size";
constexpr auto updates_label = "Install &updates when LayerC starts";
constexpr auto visual_hint = "Watches configured screen regions only while a profile needs visual matching.";
constexpr auto overlays_hint = "Screenshots, screen sharing and Magnifier cannot see a hidden window.";
constexpr auto capture_hint =
    "Hides the editor's own windows, but not the Windows file dialog, calibration, File Explorer "
    "or input-method candidate windows.";
constexpr auto text_size_hint = "Adds to the Windows text size setting.";
constexpr auto updates_hint = "The launcher checks GitHub for a signed release before it starts LayerD.";

std::string capture_hint_with_warning(const std::string_view warning) {
  if (warning.empty()) return capture_hint;
  return std::string(capture_hint) + "\n" + std::string(warning);
}

int label_width(const char *text) {
  int width = 0, height = 0;
  const auto saved = fl_draw_shortcut;
  fl_draw_shortcut = 2;
  fl_measure(text, width, height, 0);
  fl_draw_shortcut = saved;
  return width;
}

int wrapped_height(const char *text, int width) {
  int height = 0;
  fl_measure(text, width, height, 0);
  return height;
}

class OptionHint final : public Fl_Box, public PaintedTextSource {
public:
  using Fl_Box::Fl_Box;
  [[nodiscard]] std::span<const PaintedText> painted_text() const noexcept override {
    return options_painted_text;
  }
private:
  void draw() override {
    const auto ink = fixed_ink(current_editor_tokens(), painted_text().front().ink);
    labelcolor(fl_rgb_color(static_cast<uchar>(ink >> 16U),
                            static_cast<uchar>(ink >> 8U), static_cast<uchar>(ink)));
    Fl_Box::draw();
  }
};

} // namespace

OptionsWindow::OptionsWindow(const EditorOptions &initial, const EditorAppearance &appearance,
                             const int work_width, Commit commit, Applied applied, Failed failed)
    : Fl_Double_Window(1, 1, title), commit_(std::move(commit)),
      applied_(std::move(applied)), failed_(std::move(failed)) {
  fl_font(FL_HELVETICA, FL_NORMAL_SIZE);
  const int check_indent = std::min(static_cast<int>(FL_NORMAL_SIZE), 25) + 4;
  const int choice_x = margin + label_width(text_size_label) + gap;
  constexpr std::array labels{"OK", "Cancel", "&Apply"};
  std::array<int, labels.size()> button_widths{};
  int actions_width = gap * (static_cast<int>(labels.size()) - 1);
  for (std::size_t i = 0; i < labels.size(); ++i) {
    button_widths[i] = std::max(min_button_width, label_width(labels[i]) + 2 * margin);
    actions_width += button_widths[i];
  }
  const int minimum = std::max({actions_width + 2 * margin, choice_x + choice_width + margin,
                                2 * margin + check_indent + gap});
  const int width = std::clamp(
      std::max(preferred_width, label_width(overlays_label) + check_indent + 2 * margin),
      minimum, std::max(minimum, work_width));
  const int row_width = width - 2 * margin;
  int y = margin;
  const auto check = [&](const char *label, const bool checked) {
    const int w = row_width - check_indent;
    const int height = std::max(editor_input_height, wrapped_height(label, w));
    auto *control = new EditorField<Fl_Check_Button>(margin, y, row_width, height, label);
    control->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    control->value(checked);
    control->callback([](Fl_Widget *, void *data) { static_cast<OptionsWindow *>(data)->enable_apply(); },
                      this);
    y += height;
    return control;
  };
  const auto hint = [&](const int x, const char *text, int minimum_height = 0) -> Fl_Box & {
    const int w = width - margin - x;
    const int height = std::max(minimum_height, wrapped_height(text, w));
    auto *line = new OptionHint(x, y, w, height, text);
    line->align(FL_ALIGN_TOP_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    y += height + gap;
    return *line;
  };

  visual_ = check(visual_label, initial.visual_recognition);
  accessibility::describe_with(*visual_, hint(margin + check_indent, visual_hint));
  overlays_ = check(overlays_label, initial.overlay_capture_exclusion);
  accessibility::describe_with(*overlays_, hint(margin + check_indent, overlays_hint));
  editor_ = check(editor_label, initial.editor_capture_exclusion);
  const int capture_width = row_width - check_indent;
  capture_hint_ = &hint(margin + check_indent, capture_hint,
      std::max(wrapped_height(capture_hint_with_warning(capture_exclusion_incomplete).c_str(), capture_width),
               wrapped_height(capture_hint_with_warning(capture_exclusion_not_cleared).c_str(), capture_width)));
  accessibility::describe_with(*editor_, *capture_hint_);
  refresh_capture_warning();

  text_size_ = new EditorChoice(choice_x, y, choice_width, editor_input_height, text_size_label);
  text_size_->shortcut_label(1);
  for (std::size_t i = 0; i < text_scale_choices.size(); ++i) {
    const int percent = text_scale_choices[i];
    text_size_->add((std::to_string(percent) + " %").c_str());
    if (initial.text_scale == percent) text_size_->value(static_cast<int>(i));
  }
  if (text_size_->value() < 0) text_size_->value(0);
  text_size_->callback([](Fl_Widget *, void *data) { static_cast<OptionsWindow *>(data)->enable_apply(); },
                       this);
  y += editor_input_height;
  accessibility::describe_with(*text_size_, hint(choice_x, text_size_hint));

  updates_ = check(updates_label, initial.launcher_updates);
  accessibility::describe_with(*updates_, hint(margin + check_indent, updates_hint));
  y += gap;

  int x = width - margin - actions_width;
  const auto action = [&](const std::size_t i, Fl_Callback *callback) {
    auto *button = new Fl_Button(x, y, button_widths[i], editor_button_height, labels[i]);
    button->callback(callback, this);
    x += button_widths[i] + gap;
    return button;
  };
  auto *ok = action(0, [](Fl_Widget *, void *data) {
    auto &dialog = *static_cast<OptionsWindow *>(data);
    if (!dialog.apply_->active() || dialog.apply()) dialog.hide();
  });
  ok->shortcut(FL_Enter);
  primary_button(*ok);
  auto *cancel = action(1, [](Fl_Widget *, void *data) { static_cast<OptionsWindow *>(data)->hide(); });
  cancel->shortcut(FL_Escape);
  apply_ = action(2, [](Fl_Widget *, void *data) { (void)static_cast<OptionsWindow *>(data)->apply(); });
  apply_->deactivate();

  end();
  size(width, y + editor_button_height + margin);
  set_modal();
  apply_editor_appearance(appearance, *this);
}

void OptionsWindow::refresh_capture_warning() {
  const auto warning = editor_capture_exclusion_warning();
  if (capture_warning_ == warning) return;
  capture_warning_ = warning;
  auto text = capture_hint_with_warning(warning);
  capture_hint_->copy_label(text.c_str());
  accessibility::set_accessible_description(*editor_, std::move(text));
}

void OptionsWindow::enable_apply() { apply_->activate(); }

bool OptionsWindow::apply() {
  if (!apply_->active()) return true;
  const EditorOptions options{
      .visual_recognition = visual_->value() != 0,
      .overlay_capture_exclusion = overlays_->value() != 0,
      .editor_capture_exclusion = editor_->value() != 0,
      .launcher_updates = updates_->value() != 0,
      .text_scale = text_scale_choices[static_cast<std::size_t>(text_size_->value())],
  };
  if (const auto error = commit_(options)) {
    failed_(*error);
    return false;
  }
  apply_->deactivate();
  applied_(options);
  refresh_capture_warning();
  return true;
}

void show_options_dialog(EditorApp &app) {
  const auto loaded = load_editor_options(editor_capture_exclusion_requested(), current_editor_text_scale());
  auto &owner = *app.main_window();
  int x = 0, y = 0, w = 0, h = 0;
  Fl::screen_work_area(x, y, w, h, owner.screen_num());
  OptionsWindow dialog(loaded.options, app.appearance(), w - (owner.decorated_w() - owner.w()),
      [](const EditorOptions &options) { return commit_editor_options(options); },
      [&app](const EditorOptions &options) {
        apply_editor_options_live(options);
        app.report(StatusKind::success, "Options saved.");
      },
      [&app](const std::string_view error) {
        app.report(StatusKind::warning, "Could not save the selected options.");
        fl_message_title(title);
        fl_alert("%s", std::string(error).c_str());
      });
  dialog.position(
      std::clamp(owner.x() + (owner.w() - dialog.w()) / 2, x, std::max(x, x + w - dialog.w())),
      std::clamp(owner.y() + (owner.h() - dialog.h()) / 2, y, std::max(y, y + h - dialog.h())));
  dialog.show();
  if (!loaded.warning.empty()) {
    fl_message_title(title);
    fl_alert("%s", loaded.warning.data());
  }
  while (dialog.shown()) {
    Fl::wait();
    dialog.refresh_capture_warning();
  }
}

} // namespace layerc::editor
