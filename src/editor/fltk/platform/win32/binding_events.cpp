#include "platform/services.hpp"

#include "binding_controls.hpp"
#include "steady_timeout.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <commctrl.h>
#include <windowsx.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace layerc::editor {
namespace {

using Kind = FocusedKeyResult::Kind;
constexpr UINT_PTR subclass_id = 1;
// Short enough to observe a tap; it runs only while this modal owns focus.
constexpr auto controller_poll_interval = std::chrono::milliseconds{20};

FocusedKeyResult physical_key(const WPARAM key, const LPARAM data) noexcept {
  if (key == VK_PROCESSKEY || key == VK_PACKET)
    return {Kind::ignore, std::nullopt};
  if (key == VK_ESCAPE) return {Kind::cancel, std::nullopt};

  const auto bits = static_cast<std::uintptr_t>(data);
  const auto scan = (bits >> 16U) & 0xffU;
  if (scan == 0) return {Kind::key, std::nullopt};
  const auto prefix = (bits & (std::uintptr_t{1} << 24U)) ? 0xe000U : 0U;
  return {Kind::key, core::InputToken{core::InputDevice::keyboard,
                                     static_cast<std::uint16_t>(prefix | scan), 0}};
}

} // namespace

struct FocusedBindingEvents::Impl {
  explicit Impl(FocusedBindingCallbacks value) : callbacks(std::move(value)) {}
  ~Impl() { detach(); }

  [[nodiscard]] bool focused() const {
    auto *modal = Fl::modal();
    return hwnd != nullptr && modal != nullptr && modal->shown() &&
           fl_xid(modal) == hwnd && owns_native_focus(hwnd) &&
           (!callbacks.active || callbacks.active());
  }

  void attach() {
    auto *modal = Fl::modal();
    if (modal != nullptr && modal->shown()) hwnd = fl_xid(modal);
    if (hwnd == nullptr ||
        !SetWindowSubclass(hwnd, procedure, subclass_id,
                           reinterpret_cast<DWORD_PTR>(this))) {
      hwnd = nullptr;
      if (callbacks.status)
        callbacks.status("Could not listen to input in this capture dialog.");
      if (callbacks.gamepad)
        callbacks.gamepad("Gamepad capture is unavailable in this dialog.");
      return;
    }
    if (callbacks.gamepad)
      callbacks.gamepad("Gamepad: no XInput controller detected.");
    arm();
  }

  void detach() {
    disarm();
    if (const auto window = std::exchange(hwnd, nullptr))
      RemoveWindowSubclass(window, procedure, subclass_id);
  }

  void arm() {
    if (focused() && !poll_timer.owed() && !Fl::has_timeout(poll, this))
      Fl::add_timeout(0.0, poll, this);
  }

  void disarm() {
    Fl::remove_timeout(poll, this);
    poll_timer.cancel();
    controllers.reset();
    composing = false;
  }

  static void poll(void *data) {
    auto &self = *static_cast<Impl *>(data);
    if (!self.focused()) {
      self.disarm();
      return;
    }

    unsigned connected{};
    for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
      XINPUT_STATE state{};
      const bool online = XInputGetState(index, &state) == ERROR_SUCCESS;
      if (online) ++connected;
      const auto pressed =
          self.controllers.sample(index, online ? std::optional{state.Gamepad}
                                                : std::nullopt);
      for (unsigned bit = 0; bit < platform::xinput_digital_control_count; ++bit)
        if ((pressed & (std::uint32_t{1} << bit)) != 0 && self.callbacks.record)
          self.callbacks.record(platform::xinput_control_token(bit, index));
    }
    if (self.connected != connected) {
      self.connected = connected;
      if (self.callbacks.gamepad)
        self.callbacks.gamepad(
            connected == 0 ? "Gamepad: no XInput controller detected."
                           : "Gamepad: " + std::to_string(connected) +
                                 (connected == 1 ? " XInput controller ready."
                                                 : " XInput controllers ready."));
    }
    if (self.focused())
      self.poll_timer.start(controller_poll_interval);
    else
      self.disarm();
  }

  static LRESULT CALLBACK procedure(const HWND window, const UINT message,
                                    const WPARAM wparam, const LPARAM lparam,
                                    UINT_PTR, const DWORD_PTR data) {
    auto &self = *reinterpret_cast<Impl *>(data);
    if (message == WM_NCDESTROY) {
      self.detach();
      return DefSubclassProc(window, message, wparam, lparam);
    }
    if (message == WM_KILLFOCUS ||
        (message == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE) ||
        (message == WM_ACTIVATEAPP && !wparam) ||
        (message == WM_SHOWWINDOW && !wparam) ||
        (message == WM_SIZE && wparam == SIZE_MINIMIZED))
      self.disarm();

    // Keep the originating key message across FLTK's translation. FLTK may
    // replace fl_msg with WM_CHAR, and delivers those independently too.
    // Never read the text or use it to guess a physical key.
    if (message == WM_IME_STARTCOMPOSITION && self.focused())
      self.composing = true;
    const auto key = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
                             !self.composing && self.focused()
                         ? physical_key(wparam, lparam)
                         : FocusedKeyResult{Kind::ignore, std::nullopt};
    const auto previous_key = std::exchange(self.key, key);
    const auto previous_message =
        std::exchange(self.mouse_message, MouseMessage{message, wparam, lparam});
    const auto result = DefSubclassProc(window, message, wparam, lparam);
    self.mouse_message = previous_message;
    self.key = previous_key;

    if (message == WM_IME_ENDCOMPOSITION) self.composing = false;
    if (message == WM_SETFOCUS ||
        (message == WM_ACTIVATE && LOWORD(wparam) != WA_INACTIVE))
      self.arm();
    return result;
  }

  FocusedBindingCallbacks callbacks;
  SteadyTimeout poll_timer{[this] { poll(this); }};
  HWND hwnd{};
  bool composing{};
  unsigned connected{};
  FocusedControllerEdges controllers;
  FocusedKeyResult key{Kind::ignore, std::nullopt};
  struct MouseMessage {
    UINT message{};
    WPARAM wparam{};
    LPARAM lparam{};
  };
  MouseMessage mouse_message;
};

FocusedBindingEvents::FocusedBindingEvents(FocusedBindingCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(callbacks))) {}
FocusedBindingEvents::~FocusedBindingEvents() = default;

void FocusedBindingEvents::start() {
  stop();
  impl_->connected = 0;
  impl_->attach();
}

void FocusedBindingEvents::stop() { impl_->detach(); }

FocusedKeyResult FocusedBindingEvents::keyboard_token() {
  return impl_->focused() ? impl_->key
                         : FocusedKeyResult{Kind::ignore, std::nullopt};
}

bool FocusedBindingEvents::mouse_input_inside(const int x, const int y,
                                              const int w, const int h) const {
  if (!impl_->focused()) return false;
  const auto &native = impl_->mouse_message;
  switch (native.message) {
  case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
  case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
  case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
  case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
    return Fl::event_inside(x, y, w, h) != 0;
  case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    break;
  default:
    return false;
  }
  // FLTK's Win32 wheel handler does not update its local pointer coordinates.
  POINT point{GET_X_LPARAM(native.lparam), GET_Y_LPARAM(native.lparam)};
  if (!ScreenToClient(impl_->hwnd, &point)) return false;
  const auto scale = static_cast<double>(Fl::screen_scale(Fl::modal()->screen_num()));
  const auto px = static_cast<double>(point.x) / scale;
  const auto py = static_cast<double>(point.y) / scale;
  return px >= x && px < x + w && py >= y && py < y + h;
}

std::optional<core::InputToken> FocusedBindingEvents::mouse_button_token() const {
  // FLTK numbers middle before right; LayerD numbers right before middle.
  switch (Fl::event_button()) {
  case 1: return core::InputToken{core::InputDevice::mouse, 1, 0};
  case 2: return core::InputToken{core::InputDevice::mouse, 3, 0};
  case 3: return core::InputToken{core::InputDevice::mouse, 2, 0};
  case 4: return core::InputToken{core::InputDevice::mouse, 4, 0};
  case 5: return core::InputToken{core::InputDevice::mouse, 5, 0};
  default: return std::nullopt;
  }
}

std::optional<core::InputToken> FocusedBindingEvents::mouse_wheel_token() const {
  const auto &native = impl_->mouse_message;
  if (native.message != WM_MOUSEWHEEL) return std::nullopt;
  const auto delta = GET_WHEEL_DELTA_WPARAM(native.wparam);
  if (delta == 0) return std::nullopt;
  return core::InputToken{core::InputDevice::mouse,
                          static_cast<std::uint16_t>(delta > 0 ? 6 : 7), 0};
}

std::string_view focused_binding_hid_note() noexcept {
  return "Generic HID controls use a separate binding capture dialog.";
}

} // namespace layerc::editor
