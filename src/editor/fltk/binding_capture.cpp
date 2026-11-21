#include "binding_capture.hpp"
#include "profile_mutations.hpp"
#include "platform/services.hpp"

#include "editor_style.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <algorithm>
#include <string>

namespace layerc::editor {
namespace {
namespace core = layerc::core;

// A platform can suppress FLTK's translated text events, even when they
// would otherwise activate a button before reaching the dialog.
class CaptureButton final : public Fl_Button {
public:
  CaptureButton(int x, int y, int w, const char *label, FocusedBindingEvents &capture)
      : Fl_Button(x, y, w, editor_button_height, label), capture_(capture) {}
  int handle(int event) override {
    if ((event == FL_KEYDOWN || event == FL_SHORTCUT) &&
        capture_.keyboard_token().kind == FocusedKeyResult::Kind::ignore)
      return 1;
    return Fl_Button::handle(event);
  }

private:
  FocusedBindingEvents &capture_;
};

class CaptureWindow;

// Mouse buttons and the vertical wheel are read only from presses on this
// pad, so the dialog's own buttons stay usable.
class MousePad final : public EditorField<Fl_Box> {
public:
  MousePad(int x, int y, int w, int h, CaptureWindow &owner)
      : EditorField(FL_DOWN_BOX, x, y, w, h, "Mouse capture area: click a button or turn the wheel here"),
        owner_(owner) {
    align(FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
  }
  int handle(int event) override;

private:
  CaptureWindow &owner_;
};

class CaptureWindow final : public Fl_Double_Window {
public:
  CaptureWindow()
      : Fl_Double_Window(560, 300, "Capture focused binding"),
        capture({[this](const core::InputToken &token) { record(token); },
                 [this](const std::string &text) { show_status(text); },
                 [this](const std::string &text) { show_gamepad(text); },
                 [this] { return shown() != 0; }}) {
    begin();
    status = new Fl_Box(15, 10, 530, 60, "Focus this dialog and press the chord, click in the area "
        "below, or press gamepad buttons. Escape cancels; Accept records the captured chord.");
    status->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
    new MousePad(15, 75, 530, 60, *this);
    gamepad_note = new Fl_Box(15, 140, 530, 45);
    gamepad_note->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
    auto *hid = new Fl_Box(15, 190, 530, 40, focused_binding_hid_note().data());
    hid->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
    auto *clear = new CaptureButton(110, 250, 130, "Clear", capture);
    clear->callback([](Fl_Widget *, void *self) { static_cast<CaptureWindow *>(self)->reset(); }, this);
    auto *cancel = new CaptureButton(250, 250, 130, "Cancel", capture);
    cancel->callback([](Fl_Widget *, void *self) { static_cast<CaptureWindow *>(self)->hide(); }, this);
    accept = new CaptureButton(400, 250, 130, "Accept", capture);
    accept->deactivate();
    accept->callback([](Fl_Widget *, void *self) {
      auto &window = *static_cast<CaptureWindow *>(self);
      window.accepted = true; window.hide();
    }, this);
    end();
    callback([](Fl_Widget *, void *self) { static_cast<CaptureWindow *>(self)->hide(); }, this);
    set_modal();
  }
  int handle(int event) override {
    if (event == FL_KEYDOWN || event == FL_SHORTCUT) {
      const auto key = capture.keyboard_token();
      switch (key.kind) {
      case FocusedKeyResult::Kind::unhandled:
        return Fl_Double_Window::handle(event);
      case FocusedKeyResult::Kind::ignore:
        return 1;
      case FocusedKeyResult::Kind::cancel:
        hide();
        return 1;
      case FocusedKeyResult::Kind::key:
        if (key.token) record(*key.token);
        else show_status("This key's physical position is unavailable; no token was guessed.");
        return 1;
      }
    }
    return Fl_Double_Window::handle(event);
  }
  void record(const core::InputToken &token) {
    if (std::ranges::find(chord.tokens, token) == chord.tokens.end()) chord.tokens.push_back(token);
    show_status("Captured: " + chord_label(chord, {}));
    accept->activate();
  }
  void show_status(const std::string &text) { status->copy_label(text.c_str()); status->redraw(); }
  void reset() {
    chord.tokens.clear();
    accept->deactivate();
    show_status("Cleared. Press the chord again.");
  }
  void show_gamepad(const std::string &text) { gamepad_note->copy_label(text.c_str()); gamepad_note->redraw(); }

  Fl_Box *status{};
  Fl_Box *gamepad_note{};
  Fl_Button *accept{};
  core::InputChord chord;
  bool accepted{};
  FocusedBindingEvents capture; // released before the widgets it updates
};

int MousePad::handle(const int event) {
  // Let the capture area take initial focus instead of Clear, where a
  // physical Space would activate the button before reaching the dialog.
  if (event == FL_FOCUS) {
    redraw();
    return 1;
  }
  if ((event == FL_PUSH || event == FL_MOUSEWHEEL) &&
      !owner_.capture.mouse_input_inside(x(), y(), w(), h()))
    return 0;
  if (event == FL_PUSH) {
    take_focus();
    const auto token = owner_.capture.mouse_button_token();
    if (token) owner_.record(*token);
    else owner_.show_status("That pointer button has no LayerC mouse code; nothing was recorded.");
    return 1;
  }
  if (event == FL_MOUSEWHEEL) {
    const auto token = owner_.capture.mouse_wheel_token();
    if (token) owner_.record(*token);
    else owner_.show_status("Horizontal scrolling has no LayerC mouse code; nothing was recorded.");
    return 1;
  }
  return EditorField::handle(event);
}

} // namespace

std::optional<layerc::core::InputChord> capture_focused_binding() {
  CaptureWindow dialog;
  dialog.show();
  dialog.capture.start();
  while (dialog.shown()) Fl::wait();
  dialog.capture.stop();
  if (!dialog.accepted || dialog.chord.tokens.empty()) return std::nullopt;
  return dialog.chord;
}
} // namespace layerc::editor
