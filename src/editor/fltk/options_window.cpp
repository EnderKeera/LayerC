#include "options_window.hpp"

#include "accessible_widgets.hpp"
#include "appearance_window.hpp"
#include "editor_app.hpp"
#include "editor_style.hpp"
#include "kit/layout.hpp"

#include <FL/Fl_Box.H>
#include <FL/Fl_Check_Button.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <string>
#include <utility>

namespace layerc::editor {
namespace {

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

class OptionCheck final : public EditorField<Fl_Check_Button> {
public:
  using EditorField::EditorField;
private:
  void draw() override {
    const auto type = labeltype();
    labeltype(FL_NO_LABEL);
    EditorField::draw();
    labeltype(type);
    const int offset = kit::check_label_offset();
    draw_label(x() + offset, y(), w() - offset - kit::check_ring_margin(), h());
  }
};

} // namespace

OptionsWindow::OptionsWindow(const EditorOptions &initial, const EditorAppearance &appearance,
                             const int work_width, Commit commit, Applied applied, Failed failed)
    : Fl_Double_Window(1, 1, title), commit_(std::move(commit)),
      applied_(std::move(applied)), failed_(std::move(failed)) {
  auto &form = *new kit::Form;
  const auto check = [&](const char *label, const bool checked) {
    auto *control = new OptionCheck(0, 0, 0, kit::metric(kit::Metric::control_height), label);
    form.check_row(*control);
    control->value(checked);
    control->callback([](Fl_Widget *, void *data) { static_cast<OptionsWindow *>(data)->enable_apply(); },
                      this);
    return control;
  };
  visual_ = check(visual_label, initial.visual_recognition);
  form.hint(*visual_, visual_hint);
  overlays_ = check(overlays_label, initial.overlay_capture_exclusion);
  form.hint(*overlays_, overlays_hint);
  editor_ = check(editor_label, initial.editor_capture_exclusion);
  capture_hint_ = &form.hint(*editor_, capture_hint, {capture_hint_with_warning(capture_exclusion_incomplete),
                                                     capture_hint_with_warning(capture_exclusion_not_cleared)});
  refresh_capture_warning();

  text_size_ = new EditorChoice(0, 0, 0, kit::metric(kit::Metric::combo_height));
  form.field_row(text_size_label, *text_size_, choice_width);
  for (std::size_t i = 0; i < text_scale_choices.size(); ++i) {
    const int percent = text_scale_choices[i];
    text_size_->add((std::to_string(percent) + " %").c_str());
    if (initial.text_scale == percent) text_size_->value(static_cast<int>(i));
  }
  if (text_size_->value() < 0) text_size_->value(0);
  text_size_->callback([](Fl_Widget *, void *data) { static_cast<OptionsWindow *>(data)->enable_apply(); },
                       this);
  form.hint(*text_size_, text_size_hint);

  updates_ = check(updates_label, initial.launcher_updates);
  form.hint(*updates_, updates_hint);
  form.end();

  auto &buttons = *new kit::ButtonRow;
  const auto action = [&](const char *label, Fl_Callback *callback) {
    auto *button = new Fl_Button(0, 0, 0, kit::metric(kit::Metric::control_height), label);
    button->callback(callback, this);
    buttons.action(*button);
    return button;
  };
  auto *ok = action("OK", [](Fl_Widget *, void *data) {
    auto &dialog = *static_cast<OptionsWindow *>(data);
    if (!dialog.apply_->active() || dialog.apply()) dialog.hide();
  });
  ok->shortcut(FL_Enter);
  primary_button(*ok);
  auto *cancel = action("Cancel", [](Fl_Widget *, void *data) { static_cast<OptionsWindow *>(data)->hide(); });
  cancel->shortcut(FL_Escape);
  apply_ = action("&Apply", [](Fl_Widget *, void *data) { (void)static_cast<OptionsWindow *>(data)->apply(); });
  apply_->deactivate();

  buttons.end();
  end();
  frame_.emplace(*this, form, buttons, preferred_width, work_width, kit::DialogSizing::fixed);
  set_modal();
  apply_editor_appearance(appearance, *this);
}

void OptionsWindow::show() {
  Fl_Double_Window::show();
  frame_->place();
}

void OptionsWindow::refresh_capture_warning() {
  const auto warning = editor_capture_exclusion_warning();
  if (capture_warning_ == warning) return;
  capture_warning_ = warning;
  capture_hint_->copy_label(capture_hint_with_warning(warning).c_str());
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
