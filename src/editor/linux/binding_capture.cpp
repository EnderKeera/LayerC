#include "binding_capture.hpp"
#include "profile_mutations.hpp"
#include "x11_input.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/platform.H>
#include <X11/XKBlib.h>
#include <algorithm>

namespace layerc::editor {
namespace {
class CaptureWindow final : public Fl_Double_Window {
public:
  CaptureWindow() : Fl_Double_Window(560, 190, "Capture focused binding") {
    begin();
    label = new Fl_Box(15, 15, 530, 100, "Focus this dialog and press the chord.\n"
        "Escape cancels. Use Accept when the captured chord is correct.");
    label->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
    auto *cancel = new Fl_Button(250, 135, 130, 30, "Cancel");
    cancel->callback([](Fl_Widget *, void *self) { static_cast<CaptureWindow *>(self)->hide(); }, this);
    accept = new Fl_Button(400, 135, 130, 30, "Accept");
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
      auto *display = fl_x11_display();
      const auto *native = fl_xevent;
      if (!display || !native || native->type != KeyPress) return Fl_Double_Window::handle(event);
      auto *map = XkbGetKeyboard(display, XkbAllComponentsMask, XkbUseCoreKbd);
      std::optional<std::uint16_t> code;
      const auto key = native->xkey.keycode;
      if (map && map->names && map->names->keys && key >= map->min_key_code && key <= map->max_key_code)
        code = layerc::platform::xkb_key_name_to_scan_code(
            std::string_view(map->names->keys[key].name, XkbKeyNameLength));
      if (map) XkbFreeKeyboard(map, XkbAllComponentsMask, True);
      if (code) {
        const layerc::core::InputToken token{layerc::core::InputDevice::keyboard, *code, 0};
        if (std::ranges::find(chord.tokens, token) == chord.tokens.end()) chord.tokens.push_back(token);
        label->copy_label(("Captured: " + chord_label(chord, {})).c_str());
        label->redraw(); accept->activate();
      } else {
        label->copy_label("This key's physical position is unavailable; no token was guessed.");
        label->redraw();
      }
      return 1;
    }
    return Fl_Double_Window::handle(event);
  }
  Fl_Box *label{};
  Fl_Button *accept{};
  layerc::core::InputChord chord;
  bool accepted{};
};
}
std::optional<layerc::core::InputChord> capture_focused_binding() {
  CaptureWindow dialog;
  dialog.show();
  while (dialog.shown()) Fl::wait();
  if (!dialog.accepted || dialog.chord.tokens.empty()) return std::nullopt;
  return dialog.chord;
}
}
