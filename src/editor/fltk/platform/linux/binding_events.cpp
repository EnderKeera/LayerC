#include "platform/services.hpp"
#include "x11_error.hpp"
#include "x11_input.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Widget.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput2.h>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace layerc::editor {
namespace {
namespace core = layerc::core;
namespace platform = layerc::platform;

// The profile format names XInput buttons 1-10 and 13-18 for the first four
// controllers; any other gamepad button has no token to record.
bool controller_token_valid(const core::InputToken &token) {
  return token.controller_index <= 3 &&
         ((token.code >= 1 && token.code <= 10) || (token.code >= 13 && token.code <= 18));
}

class CaptureOwner final {
public:
  explicit CaptureOwner(FocusedBindingCallbacks callbacks) : callbacks_(std::move(callbacks)) {}
  void attach() { target_ = std::make_unique<Fl_Widget_Tracker>(Fl::modal()); }
  void detach() { target_.reset(); }
  [[nodiscard]] Fl_Window *window() const {
    return target_ ? static_cast<Fl_Window *>(target_->widget()) : nullptr;
  }
  [[nodiscard]] bool shown() const {
    auto *const target = window();
    return target && target->shown() && (!callbacks_.active || callbacks_.active());
  }
  void record(const core::InputToken &token) {
    if (callbacks_.record) callbacks_.record(token);
  }
  void show_status(const std::string &text) {
    if (callbacks_.status) callbacks_.status(text);
  }
  void show_gamepad(const std::string &text) {
    if (callbacks_.gamepad) callbacks_.gamepad(text);
  }
private:
  FocusedBindingCallbacks callbacks_;
  std::unique_ptr<Fl_Widget_Tracker> target_;
};

// Gamepad buttons arrive as XI2 raw events selected on the root window only
// while the dialog is open, and are recorded only while this dialog holds the
// X input focus. Reads only; the selection is withdrawn on close.
class GamepadListener final {
public:
  GamepadListener(CaptureOwner &owner) : owner_(owner) {}
  GamepadListener(const GamepadListener &) = delete;
  GamepadListener &operator=(const GamepadListener &) = delete;
  ~GamepadListener() { stop(); }

  // Empty on success, otherwise why gamepad buttons cannot be captured.
  std::string start();
  void stop();
  [[nodiscard]] std::string describe() const;

private:
  static int on_event(void *event, void *data);
  int handle(XEvent &event);
  void select(bool enable);
  void refresh() {
    if (auto devices = platform::query_input_devices(display_)) gamepads_ = platform::gamepad_ids(*devices);
  }

  CaptureOwner &owner_;
  Display *display_{};
  int opcode_{};
  bool selected_{};
  std::vector<int> gamepads_;
  std::string unavailable_;
};

// Mapping-only Display connection to the same server as FLTK. XkbSelectEvents
// cannot report FLTK's prior mask, so selecting on the shared connection could
// destroy its subscriptions; closing this owned connection drops only ours.
// No polling, grab or key/button selection happens here.
class KeyboardMapListener final {
public:
  explicit KeyboardMapListener(CaptureOwner &owner) : owner_(owner) {}
  KeyboardMapListener(const KeyboardMapListener &) = delete;
  KeyboardMapListener &operator=(const KeyboardMapListener &) = delete;
  ~KeyboardMapListener() { stop(); }
  [[nodiscard]] std::string start();
  void stop();
  [[nodiscard]] std::optional<std::uint16_t> scan_code(std::uint32_t keycode);
private:
  static void readable(int, void *data);
  void drain();
  CaptureOwner &owner_;
  Display *display_{};
  int event_base_{};
  bool watching_{};
  platform::X11PhysicalKeymap keymap_;
};

std::string KeyboardMapListener::start() {
  stop();
  auto *shared = fl_x11_display();
  if (shared == nullptr)
    return "Keyboard: the X display is unavailable; mouse/gamepad capture is unchanged.";
  display_ = XOpenDisplay(DisplayString(shared));
  if (display_ == nullptr)
    return "Keyboard: the physical-map connection is unavailable; mouse/gamepad capture is unchanged.";
  int opcode{}, error{};
  int major = XkbMajorVersion, minor = XkbMinorVersion;
  bool subscribed{};
  {
    platform::X11ErrorTrap trap{display_};
    subscribed = XkbQueryExtension(display_, &opcode, &event_base_, &error,
                                   &major, &minor) != 0;
    if (subscribed) {
      constexpr unsigned int events = XkbNewKeyboardNotifyMask |
          XkbMapNotifyMask | XkbNamesNotifyMask;
      subscribed = XkbSelectEvents(display_, XkbUseCoreKbd, events, events) != 0;
    }
    subscribed = trap.ok() && subscribed;
  }
  if (!subscribed) {
    stop();
    return "Keyboard: XKB map notifications are unavailable; no keyboard token will be guessed.";
  }
  (void)keymap_.refresh(display_);
  Fl::add_fd(ConnectionNumber(display_), FL_READ, &KeyboardMapListener::readable, this);
  watching_ = true;
  drain(); // Xlib synchronous queries can buffer notices without fd readiness.
  return keymap_.available() ? std::string{} :
      "Keyboard: XKB physical key names are unavailable; no keyboard token will be guessed.";
}

void KeyboardMapListener::stop() {
  if (display_ != nullptr) {
    if (watching_) Fl::remove_fd(ConnectionNumber(display_));
    watching_ = false;
    XCloseDisplay(display_);
    display_ = nullptr;
  }
  event_base_ = 0;
  (void)keymap_.refresh(nullptr);
}

void KeyboardMapListener::readable(int, void *data) {
  static_cast<KeyboardMapListener *>(data)->drain();
}

void KeyboardMapListener::drain() {
  if (display_ == nullptr) return;
  for (;;) {
    bool changed{};
    while (XPending(display_) != 0) {
      XEvent event{};
      XNextEvent(display_, &event);
      if (event.type == MappingNotify) {
        XRefreshKeyboardMapping(&event.xmapping);
        changed = true;
      } else if (event.type == event_base_) {
        const auto &xkb = *reinterpret_cast<const XkbEvent *>(&event);
        changed = changed || xkb.any.xkb_type == XkbNewKeyboardNotify ||
            xkb.any.xkb_type == XkbMapNotify || xkb.any.xkb_type == XkbNamesNotify;
      }
    }
    if (!changed) return;
    if (!keymap_.refresh(display_))
      owner_.show_status("Keyboard: XKB physical key names are unavailable; no token was guessed.");
    // refresh can synchronously buffer another notice. Drain that before
    // returning to a loop that otherwise blocks on socket readability.
  }
}

std::optional<std::uint16_t>
KeyboardMapListener::scan_code(const std::uint32_t keycode) {
  drain(); // Refresh queued changes before converting a focused keypress.
  return keymap_.scan_code(keycode);
}

std::string GamepadListener::start() {
  display_ = fl_x11_display();
  int event{}, error{};
  if (!display_ || !XQueryExtension(display_, "XInputExtension", &opcode_, &event, &error))
    return unavailable_ = "XInput 2 is unavailable on this display";
  int major = 2, minor = 0;
  if (XIQueryVersion(display_, &major, &minor) != Success)
    return unavailable_ = "the X server does not offer XInput 2";
  refresh();
  select(true);
  Fl::add_system_handler(&GamepadListener::on_event, this);
  return {};
}

void GamepadListener::stop() {
  if (!display_) return;
  if (selected_) {
    Fl::remove_system_handler(&GamepadListener::on_event);
    select(false);
  }
  display_ = nullptr;
}

void GamepadListener::select(const bool enable) {
  unsigned char mask[(XI_LASTEVENT + 7) / 8] = {};
  if (enable) { XISetMask(mask, XI_RawButtonPress); XISetMask(mask, XI_HierarchyChanged); }
  XIEventMask event_mask{XIAllDevices, static_cast<int>(sizeof mask), mask};
  XISelectEvents(display_, DefaultRootWindow(display_), &event_mask, 1);
  XFlush(display_);
  selected_ = enable;
}

std::string GamepadListener::describe() const {
  if (!unavailable_.empty()) return "Gamepad: " + unavailable_ + "; controller buttons cannot be captured.";
  if (gamepads_.empty()) return "Gamepad: no gamepad-class XInput 2 device is visible; connect one to capture its buttons.";
  return "Gamepad: " + std::to_string(gamepads_.size()) +
         " gamepad-class device(s) visible; press a button while this dialog is focused.";
}

int GamepadListener::on_event(void *event, void *data) {
  return static_cast<GamepadListener *>(data)->handle(*static_cast<XEvent *>(event));
}

int GamepadListener::handle(XEvent &event) {
  if (!selected_ || event.type != GenericEvent || event.xgeneric.extension != opcode_) return 0;
  if (!XGetEventData(display_, &event.xcookie)) return 1;
  const int type = event.xcookie.evtype;
  int deviceid{};
  std::uint32_t detail{};
  if (type == XI_RawButtonPress) {
    const auto &raw = *static_cast<const XIRawEvent *>(event.xcookie.data);
    deviceid = raw.deviceid;
    detail = static_cast<std::uint32_t>(raw.detail);
  }
  XFreeEventData(display_, &event.xcookie);
  if (type == XI_HierarchyChanged) {
    refresh();
    owner_.show_gamepad(describe());
    return 1;
  }
  if (type != XI_RawButtonPress || std::ranges::find(gamepads_, deviceid) == gamepads_.end()) return 1;
  ::Window focus{};
  int revert{};
  XGetInputFocus(display_, &focus, &revert);
  if (!owner_.shown() || focus != fl_xid(owner_.window())) return 1; // focused input only
  const auto token = platform::gamepad_button_token(gamepads_, deviceid, detail);
  if (controller_token_valid(token)) owner_.record(token);
  else owner_.show_status("Gamepad button " + std::to_string(detail) +
                          " has no LayerC controller code (1-10, 13-18; first four gamepads).");
  return 1;
}
} // namespace

struct FocusedBindingEvents::Impl {
  CaptureOwner owner;
  KeyboardMapListener keyboard{owner};
  GamepadListener gamepad{owner};
  explicit Impl(FocusedBindingCallbacks callbacks) : owner(std::move(callbacks)) {}
};

FocusedBindingEvents::FocusedBindingEvents(FocusedBindingCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(callbacks))) {}
FocusedBindingEvents::~FocusedBindingEvents() = default;

void FocusedBindingEvents::start() {
  stop();
  impl_->owner.attach();
  if (const auto error = impl_->keyboard.start(); !error.empty())
    impl_->owner.show_status(error);
  (void)impl_->gamepad.start();
  impl_->owner.show_gamepad(impl_->gamepad.describe());
}

void FocusedBindingEvents::stop() {
  impl_->keyboard.stop();
  impl_->gamepad.stop();
  impl_->owner.detach();
}

FocusedKeyResult FocusedBindingEvents::keyboard_token() {
  const auto *native = fl_xevent;
  if (!fl_x11_display() || !native || native->type != KeyPress) return {};
  const auto code = impl_->keyboard.scan_code(native->xkey.keycode);
  if (!code) return {true, std::nullopt};
  return {true, core::InputToken{core::InputDevice::keyboard, *code, 0}};
}

std::optional<core::InputToken> FocusedBindingEvents::mouse_button_token() const {
  const auto *native = fl_xevent;
  const auto code = native && native->type == ButtonPress
      ? platform::button_to_mouse_code(native->xbutton.button) : std::uint16_t{};
  if (code == 0) return std::nullopt;
  return core::InputToken{core::InputDevice::mouse, code, 0};
}

} // namespace layerc::editor
