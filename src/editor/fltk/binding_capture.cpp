#include "binding_capture.hpp"
#include "profile_mutations.hpp"
#include "platform/services.hpp"

#include "editor_style.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <algorithm>
#include <cstdint>
#include <string>

namespace layerc::editor {
namespace {
namespace core = layerc::core;

class CaptureWindow;

// Mouse buttons and the vertical wheel are read only from presses on this
// pad, so the dialog's own buttons stay usable.
class MousePad final : public Fl_Box {
public:
  MousePad(int x, int y, int w, int h, CaptureWindow &owner)
      : Fl_Box(FL_DOWN_BOX, x, y, w, h, "Mouse capture area: click a button or turn the wheel here"),
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
    auto *hid = new Fl_Box(15, 190, 530, 40, "Generic HID capture is unavailable on Linux; "
        "existing HID bindings are kept unchanged.");
    hid->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
    auto *clear = new Fl_Button(110, 250, 130, editor_button_height, "Clear");
    clear->callback([](Fl_Widget *, void *self) { static_cast<CaptureWindow *>(self)->reset(); }, this);
    auto *cancel = new Fl_Button(250, 250, 130, editor_button_height, "Cancel");
    cancel->callback([](Fl_Widget *, void *self) { static_cast<CaptureWindow *>(self)->hide(); }, this);
    accept = new Fl_Button(400, 250, 130, editor_button_height, "Accept");
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
      if (Fl::event_key() == FL_Escape) { hide(); return 1; }
      const auto key = capture.keyboard_token();
      if (!key.handled) return Fl_Double_Window::handle(event);
      if (key.token) record(*key.token);
      else show_status("This key's physical position is unavailable; no token was guessed.");
      return 1;
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
  if (event == FL_PUSH) {
    const auto token = owner_.capture.mouse_button_token();
    if (token) owner_.record(*token);
    else owner_.show_status("That pointer button has no LayerC mouse code; nothing was recorded.");
    return 1;
  }
  if (event == FL_MOUSEWHEEL) {
    if (Fl::event_dy() != 0)
      owner_.record({core::InputDevice::mouse, static_cast<std::uint16_t>(Fl::event_dy() < 0 ? 6 : 7), 0});
    else owner_.show_status("Horizontal scrolling has no LayerC mouse code; nothing was recorded.");
    return 1;
  }
  return Fl_Box::handle(event);
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
