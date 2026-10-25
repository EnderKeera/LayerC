#include "binding_capture.hpp"
#include "test_support.hpp"

#include "layerc/core/profile.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Widget.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <functional>
#include <iterator>
#include <string_view>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef LAYERC_TEST_XTEST

namespace {

using layerc::test::expect;

constexpr const char *capture_title = "Capture focused binding";

Fl_Widget *find_label(Fl_Widget &root, const std::string_view label) {
  if (root.label() != nullptr && std::string_view{root.label()} == label) return &root;
  if (auto *group = dynamic_cast<Fl_Group *>(&root)) {
    for (int index = 0; index < group->children(); ++index)
      if (auto *found = find_label(*group->child(index), label)) return found;
  }
  return nullptr;
}

Fl_Widget *button(Fl_Window &modal, const std::string_view label) {
  auto *found = find_label(modal, label);
  expect(found != nullptr, "the focused capture dialog owns the named button");
  return found != nullptr ? found : &modal;
}

using KeyName = std::array<char, XkbKeyNameLength>;

std::vector<KeyName> snapshot_key_names(Display *display) {
  std::vector<KeyName> names;
  auto *keyboard = XkbGetMap(display, 0, XkbUseCoreKbd);
  if (!expect(keyboard != nullptr, "the complete physical key-name table is readable"))
    return names;
  const auto status = XkbGetNames(display, XkbKeyNamesMask, keyboard);
  if (expect(status == Success && keyboard->names != nullptr &&
                 keyboard->names->keys != nullptr,
             "the complete physical key-name table is populated")) {
    for (unsigned int keycode = keyboard->min_key_code;
         keycode <= keyboard->max_key_code; ++keycode) {
      KeyName name{};
      const auto &source = keyboard->names->keys[keycode].name;
      std::copy(std::begin(source), std::end(source), name.begin());
      names.push_back(name);
    }
  }
  XkbFreeKeyboard(keyboard, XkbAllComponentsMask, True);
  return names;
}

struct ModalState {
  const char *title{};
  std::exception_ptr failure;
  bool delivered{};
  int attempts{};
  std::function<void(Fl_Window &)> drive;
};

void drive_modal(void *data) {
  auto &state = *static_cast<ModalState *>(data);
  try {
    auto *modal = dynamic_cast<Fl_Window *>(Fl::modal());
    if (modal == nullptr || std::string_view{modal->label() == nullptr ? "" : modal->label()} !=
                                state.title ||
        !modal->shown()) {
      if (++state.attempts < 100) {
        Fl::add_timeout(.01, drive_modal, data);
        return;
      }
      expect(false, "the focused capture modal becomes visible");
      state.delivered = true;
      return;
    }
    auto *display = fl_x11_display();
    if (display == nullptr) {
      expect(false, "the disposable X display is available");
      state.delivered = true;
      modal->hide();
      return;
    }
    XSetInputFocus(display, fl_xid(modal), RevertToParent, CurrentTime);
    XSync(display, False);
    state.drive(*modal);
    state.delivered = true;
  } catch (...) {
    state.failure = std::current_exception();
    if (auto *modal = Fl::modal()) static_cast<Fl_Window *>(modal)->hide();
    state.delivered = true;
  }
}

// Releases injected keys and restores a renamed XKB key name on every path.
class KeyDriver final {
public:
  explicit KeyDriver(Display *display) : display_(display) {}
  ~KeyDriver() {
    if (display_ == nullptr) return;
    release_all();
    restore_name();
  }
  KeyDriver(const KeyDriver &) = delete;
  KeyDriver &operator=(const KeyDriver &) = delete;

  void press(const KeySym symbol) {
    const auto keycode = XKeysymToKeycode(display_, symbol);
    expect(keycode != 0, "the disposable server resolves the requested key");
    if (keycode == 0) return;
    XTestFakeKeyEvent(display_, keycode, True, CurrentTime);
    XSync(display_, False);
    for (int attempt = 0; attempt < 50; ++attempt) Fl::check();
    held_.push_back(keycode);
  }
  void release_all() {
    for (auto it = held_.rbegin(); it != held_.rend(); ++it) {
      XTestFakeKeyEvent(display_, *it, False, CurrentTime);
      XSync(display_, False);
      for (int attempt = 0; attempt < 50; ++attempt) Fl::check();
    }
    held_.clear();
  }
  void rename_key(const KeySym symbol, const char (&name)[5]) {
    auto *keyboard = XkbGetMap(display_, 0, XkbUseCoreKbd);
    if (!expect(keyboard != nullptr, "the server key map is readable")) return;
    if (XkbGetNames(display_, XkbKeyNamesMask, keyboard) == Success &&
        keyboard->names != nullptr && keyboard->names->keys != nullptr) {
      const auto keycode = XKeysymToKeycode(display_, symbol);
      const auto &original = keyboard->names->keys[keycode];
      std::copy(std::begin(original.name), std::end(original.name), original_name_);
      keycode_ = keycode;
      has_original_ = true;
      std::copy(std::begin(name), std::end(name) - 1, keyboard->names->keys[keycode].name);
      expect(XkbSetNames(display_, XkbKeyNamesMask, 0, 0, keyboard) != 0,
             "the focused test renames the physical key name");
      XSync(display_, False);
    }
    XkbFreeKeyboard(keyboard, XkbAllComponentsMask, True);
  }
  void restore_name() {
    if (!has_original_) return;
    auto *keyboard = XkbGetMap(display_, 0, XkbUseCoreKbd);
    if (keyboard != nullptr) {
      if (XkbGetNames(display_, XkbKeyNamesMask, keyboard) == Success &&
          keyboard->names != nullptr && keyboard->names->keys != nullptr) {
        std::copy(std::begin(original_name_), std::end(original_name_),
                  keyboard->names->keys[keycode_].name);
        (void)XkbSetNames(display_, XkbKeyNamesMask, 0, 0, keyboard);
        XSync(display_, False);
      }
      XkbFreeKeyboard(keyboard, XkbAllComponentsMask, True);
    }
    has_original_ = false;
  }

private:
  Display *display_{};
  std::vector<unsigned int> held_;
  bool has_original_{};
  unsigned int keycode_{};
  char original_name_[XkbKeyNameLength]{};
};

std::optional<layerc::core::InputChord>
run_capture(const std::function<void(Fl_Window &, KeyDriver &)> &drive, bool &delivered) {
  ModalState state;
  state.title = capture_title;
  state.drive = [&](Fl_Window &modal) {
    auto *display = fl_x11_display();
    KeyDriver keys(display);
    drive(modal, keys);
  };
  Fl::add_timeout(.01, drive_modal, &state);
  auto chord = layerc::editor::capture_focused_binding();
  Fl::remove_timeout(drive_modal, &state);
  delivered = state.delivered;
  if (state.failure) std::rethrow_exception(state.failure);
  return chord;
}

std::size_t open_fds() {
  return static_cast<std::size_t>(std::distance(
      std::filesystem::directory_iterator("/proc/self/fd"),
      std::filesystem::directory_iterator{}));
}

} // namespace

void linux_editor_focused_keyboard_capture_contracts() {
  layerc::test::require_x11();
  layerc::test::ScopedEnvironment backend("FLTK_BACKEND",
                                           std::filesystem::path("x11"));
  using layerc::core::InputDevice;
  using layerc::core::InputToken;

  // 1. A real focused modal records exactly the physical K token.
  {
    bool delivered{};
    const auto chord = run_capture([](Fl_Window &modal, KeyDriver &keys) {
      keys.press(XK_k);
      keys.release_all();
      expect(button(modal, "Accept")->active(),
             "a real key event activates Accept");
      button(modal, "Accept")->do_callback();
    }, delivered);
    expect(delivered && chord && chord->tokens == std::vector<InputToken>{{InputDevice::keyboard, 0x25, 0}},
           "focused capture returns exactly the physical K token");
  }

  // 2. A chord keeps press order and needs real events before Accept.
  {
    bool delivered{};
    const auto chord = run_capture([](Fl_Window &modal, KeyDriver &keys) {
      expect(!button(modal, "Accept")->active(),
             "Accept starts inactive before any real key event");
      keys.press(XK_Control_L);
      keys.press(XK_k);
      keys.release_all();
      expect(button(modal, "Accept")->active(),
             "Accept activates only after the real chord");
      button(modal, "Accept")->do_callback();
    }, delivered);
    expect(delivered && chord &&
               chord->tokens == std::vector<InputToken>{
                                    {InputDevice::keyboard, 0x1D, 0},
                                    {InputDevice::keyboard, 0x25, 0}},
           "focused capture keeps the physical chord in press order");
  }

  // 3. A live physical-name change invalidates the cache and never guesses.
  {
    auto *display = fl_x11_display();
    if (expect(display != nullptr, "the disposable X display is available")) {
      const auto names_before = snapshot_key_names(display);
      expect(!names_before.empty(), "the disposable display has physical key names");
      bool delivered{};
      const auto chord = run_capture([&](Fl_Window &modal, KeyDriver &keys) {
        keys.rename_key(XK_k, "ZZZZ");
        XSync(display, False);
        keys.press(XK_k);
        keys.release_all();
        expect(!button(modal, "Accept")->active(),
               "an unknown physical name never activates Accept");
        bool found{};
        if (auto *group = dynamic_cast<Fl_Group *>(&modal)) {
          for (int index = 0; index < group->children() && !found; ++index) {
            auto *child = dynamic_cast<Fl_Box *>(group->child(index));
            if (child != nullptr && child->label() != nullptr &&
                std::string_view{child->label()}.find("no token was guessed") !=
                    std::string_view::npos)
              found = true;
          }
        }
        expect(found,
               "the focused dialog explains the unknown physical position");
        button(modal, "Cancel")->do_callback();
      }, delivered);
      expect(delivered && !chord,
             "a renamed physical key is refused without a fallback token");
      expect(snapshot_key_names(display) == names_before,
             "renaming a physical key restores the complete key-name table");
    }
    bool delivered{};
    const auto recovered = run_capture([](Fl_Window &modal, KeyDriver &keys) {
      keys.press(XK_k);
      keys.release_all();
      button(modal, "Accept")->do_callback();
    }, delivered);
    expect(delivered && recovered &&
               recovered->tokens == std::vector<InputToken>{{InputDevice::keyboard, 0x25, 0}},
           "the shared physical map recovers after the name is restored");
  }

  // 4. Focused capture owns no leaked descriptors after the first warm-up.
  {
    bool delivered{};
    (void)run_capture([](Fl_Window &modal, KeyDriver &keys) {
      keys.press(XK_k);
      keys.release_all();
      button(modal, "Accept")->do_callback();
    }, delivered);
    Fl::check();
    const auto after_first = open_fds();
    for (int index = 0; index < 3; ++index) {
      bool again{};
      (void)run_capture([](Fl_Window &modal, KeyDriver &keys) {
        keys.press(XK_k);
        keys.release_all();
        button(modal, "Accept")->do_callback();
      }, again);
      Fl::check();
    }
    expect(open_fds() == after_first,
           "repeated focused capture modals release every owned connection");
  }
}

#endif // LAYERC_TEST_XTEST
