// Passive XI2 raw input (V2.6.1): raw key/button events on the root window,
// normalized into the core input token space, plus hierarchy tracking for
// hotplug. Reads only — no event is ever synthesized back into the server.
#include "x11_input.hpp"

#include "x11_error.hpp"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace layerc::platform {
namespace {

struct KeysymVk {
  std::uint32_t keysym;
  std::uint16_t vk;
};

// Latin-1 letters and digits share their VK with the uppercase character;
// everything else needs an explicit entry.
constexpr KeysymVk special_keys[] = {
    {XK_Escape, 0x1B},      {XK_Tab, 0x09},         {XK_KP_Tab, 0x09},
    {XK_Return, 0x0D},      {XK_KP_Enter, 0x0D},    {XK_space, 0x20},
    {XK_KP_Space, 0x20},    {XK_BackSpace, 0x08},   {XK_Delete, 0x2E},
    {XK_KP_Delete, 0x2E},   {XK_Insert, 0x2D},      {XK_KP_Insert, 0x2D},
    {XK_Home, 0x24},        {XK_KP_Home, 0x24},     {XK_End, 0x23},
    {XK_KP_End, 0x23},      {XK_Prior, 0x21},       {XK_KP_Prior, 0x21},
    {XK_Next, 0x22},        {XK_KP_Next, 0x22},     {XK_Left, 0x25},
    {XK_KP_Left, 0x25},     {XK_Up, 0x26},          {XK_KP_Up, 0x26},
    {XK_Right, 0x27},       {XK_KP_Right, 0x27},    {XK_Down, 0x28},
    {XK_KP_Down, 0x28},     {XK_KP_Begin, 0x0C},    {XK_Print, 0x2C},
    {XK_Scroll_Lock, 0x91}, {XK_Pause, 0x13},       {XK_Break, 0x13},
    {XK_Num_Lock, 0x90},    {XK_Caps_Lock, 0x14},   {XK_Shift_L, 0x10},
    {XK_Shift_R, 0x10},     {XK_Control_L, 0x11},   {XK_Control_R, 0x11},
    {XK_Alt_L, 0x12},       {XK_Alt_R, 0x12},       {XK_Super_L, 0x5B},
    {XK_Super_R, 0x5C},     {XK_Menu, 0x5D},        {XK_Help, 0x2F},
    {XK_Cancel, 0x03},      {XK_KP_Multiply, 0x6A}, {XK_KP_Add, 0x6B},
    {XK_KP_Separator, 0x6C}, {XK_KP_Subtract, 0x6D},
    {XK_KP_Decimal, 0x6E},  {XK_KP_Divide, 0x6F},   {XK_KP_0, 0x60},
    {XK_KP_1, 0x61},        {XK_KP_2, 0x62},        {XK_KP_3, 0x63},
    {XK_KP_4, 0x64},        {XK_KP_5, 0x65},        {XK_KP_6, 0x66},
    {XK_KP_7, 0x67},        {XK_KP_8, 0x68},        {XK_KP_9, 0x69},
    // OEM punctuation: VK_OEM_1 .. VK_OEM_7 in keyboard order.
    {XK_semicolon, 0xBA},   {XK_equal, 0xBB},       {XK_comma, 0xBC},
    {XK_minus, 0xBD},       {XK_period, 0xBE},      {XK_slash, 0xBF},
    {XK_grave, 0xC0},       {XK_bracketleft, 0xDB}, {XK_backslash, 0xDC},
    {XK_bracketright, 0xDD}, {XK_apostrophe, 0xDE},
};

// Device names are free-form; these tokens separate a pointing device from a
// gamepad when the class list cannot (both are buttons plus axes).
constexpr std::string_view pointer_names[] = {
    "pointer", "mouse", "touchpad", "trackpad", "trackpoint", "touchscreen",
    "pen",     "tablet", "xtest",
};

bool is_pointer_name(const std::string_view name) noexcept {
  std::string lower;
  lower.reserve(name.size());
  for (const char c : name)
    lower.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  for (const auto needle : pointer_names)
    if (lower.find(needle) != std::string::npos) return true;
  return false;
}

} // namespace

X11DeviceKind classify_device(std::string_view name, const bool has_keys,
                              const bool has_buttons, const int valuators,
                              const bool has_scroll) noexcept {
  if (has_keys) return X11DeviceKind::keyboard;
  if (!has_buttons) return X11DeviceKind::other;
  if (has_scroll) return X11DeviceKind::mouse;
  if (is_pointer_name(name)) return X11DeviceKind::mouse;
  if (valuators >= 2) return X11DeviceKind::gamepad;
  return X11DeviceKind::other;
}

std::optional<std::uint16_t> keysym_to_vk(const std::uint32_t keysym) noexcept {
  if (keysym >= 0x61 && keysym <= 0x7A)
    return static_cast<std::uint16_t>(keysym - 0x20U); // a-z -> VK_A..VK_Z
  if (keysym >= 0x41 && keysym <= 0x5A) return static_cast<std::uint16_t>(keysym);
  if (keysym >= 0x30 && keysym <= 0x39) return static_cast<std::uint16_t>(keysym);
  if (keysym >= XK_F1 && keysym <= XK_F24)
    return static_cast<std::uint16_t>(0x70U + (keysym - XK_F1));
  for (const auto &entry : special_keys)
    if (entry.keysym == keysym) return entry.vk;
  return std::nullopt;
}

std::uint16_t button_to_mouse_code(const std::uint32_t button) noexcept {
  switch (button) {
  case 1: return 1;
  case 2: return 2;
  case 3: return 3;
  case 4: return 6; // X wheel up -> profile wheel up
  case 5: return 7; // X wheel down -> profile wheel down
  case 6: return 4; // X side button -> profile X1
  case 7: return 5; // X side button -> profile X2
  default: return button > 0xFFFFU ? 0 : static_cast<std::uint16_t>(button);
  }
}

X11InputSource::X11InputSource(struct _XDisplay *display, const int xi_opcode,
                               const std::uint64_t root) noexcept
    : display_(display), xi_opcode_(xi_opcode), root_(root) {}

X11InputSource::X11InputSource(X11InputSource &&other) noexcept
    : display_(std::exchange(other.display_, nullptr)),
      xi_opcode_(std::exchange(other.xi_opcode_, 0)),
      root_(std::exchange(other.root_, 0)), devices_(std::move(other.devices_)),
      gamepads_(std::move(other.gamepads_)) {}

X11InputSource &X11InputSource::operator=(X11InputSource &&other) noexcept {
  if (this != &other) {
    deselect_events();
    display_ = std::exchange(other.display_, nullptr);
    xi_opcode_ = std::exchange(other.xi_opcode_, 0);
    root_ = std::exchange(other.root_, 0);
    devices_ = std::move(other.devices_);
    gamepads_ = std::move(other.gamepads_);
  }
  return *this;
}

X11InputSource::~X11InputSource() { deselect_events(); }

std::optional<X11InputSource>
X11InputSource::create(const X11Display &display, std::string &error) {
  error.clear();
  if (!display.valid()) {
    error = "xi2: no X display to select input on";
    return std::nullopt;
  }
  if (!display.probe().xi2.present || display.probe().xi2.opcode == 0) {
    error = "xi2: XInput 2.0 is not available on this server";
    return std::nullopt;
  }
  X11InputSource source(display.raw(), display.probe().xi2.opcode,
                        display.root());
  X11ErrorTrap trap{display.raw()};
  source.select_events();
  if (!trap.ok()) {
    error = "xi2: selecting raw events on the root window failed";
    return std::nullopt;
  }
  (void)source.refresh_devices();
  return source;
}

void X11InputSource::select_events() noexcept {
  auto *raw = reinterpret_cast<Display *>(display_);
  unsigned char mask[(XI_LASTEVENT + 7) / 8] = {};
  XISetMask(mask, XI_RawKeyPress);
  XISetMask(mask, XI_RawKeyRelease);
  XISetMask(mask, XI_RawButtonPress);
  XISetMask(mask, XI_RawButtonRelease);
  XISetMask(mask, XI_RawMotion);
  XISetMask(mask, XI_HierarchyChanged);
  XIEventMask event_mask{};
  // Raw events are emitted by slave devices only, so selecting every device
  // yields each physical event once and still reaches unattached gamepads.
  event_mask.deviceid = XIAllDevices;
  event_mask.mask_len = sizeof(mask);
  event_mask.mask = mask;
  XISelectEvents(raw, static_cast<::Window>(root_), &event_mask, 1);
  XSync(raw, False);
}

void X11InputSource::deselect_events() noexcept {
  if (display_ == nullptr) return;
  auto *raw = reinterpret_cast<Display *>(display_);
  unsigned char mask[(XI_LASTEVENT + 7) / 8] = {};
  XIEventMask event_mask{};
  event_mask.deviceid = XIAllDevices;
  event_mask.mask_len = sizeof(mask);
  event_mask.mask = mask;
  X11ErrorTrap trap{raw};
  XISelectEvents(raw, static_cast<::Window>(root_), &event_mask, 1);
  (void)trap.ok();
  display_ = nullptr;
}

X11DeviceKind X11InputSource::kind_of(const int deviceid) const noexcept {
  for (const auto &device : devices_)
    if (device.id == deviceid) return device.kind;
  return X11DeviceKind::other;
}

bool X11InputSource::refresh_devices() noexcept {
  if (display_ == nullptr) return false;
  auto *raw = reinterpret_cast<Display *>(display_);
  int count{};
  X11ErrorTrap trap{raw};
  XIDeviceInfo *info = XIQueryDevice(raw, XIAllDevices, &count);
  if (info == nullptr || !trap.ok()) {
    if (info != nullptr) XIFreeDeviceInfo(info);
    return false;
  }
  std::vector<X11InputDevice> next;
  std::vector<int> gamepads;
  for (int index = 0; index < count; ++index) {
    const XIDeviceInfo &device = info[index];
    // Master devices are virtual: they never generate raw events.
    if (device.use == XIMasterPointer || device.use == XIMasterKeyboard)
      continue;
    bool has_keys{};
    bool has_buttons{};
    bool has_scroll{};
    int valuators{};
    for (int slot = 0; slot < device.num_classes; ++slot) {
      switch (device.classes[slot]->type) {
      case XIKeyClass: has_keys = true; break;
      case XIButtonClass: has_buttons = true; break;
      case XIValuatorClass: ++valuators; break;
      case XIScrollClass: has_scroll = true; break;
      default: break;
      }
    }
    X11InputDevice entry;
    entry.id = device.deviceid;
    entry.name = device.name != nullptr ? device.name : "";
    entry.kind = classify_device(entry.name, has_keys, has_buttons, valuators,
                                 has_scroll);
    next.push_back(std::move(entry));
  }
  XIFreeDeviceInfo(info);
  std::ranges::sort(next, {}, &X11InputDevice::id);
  for (const auto &device : next)
    if (device.kind == X11DeviceKind::gamepad) gamepads.push_back(device.id);

  const bool changed = next != devices_ || gamepads != gamepads_;
  devices_ = std::move(next);
  gamepads_ = std::move(gamepads);
  return changed;
}

X11InputCapabilities X11InputSource::capabilities() const noexcept {
  X11InputCapabilities caps;
  for (const auto &device : devices_) {
    switch (device.kind) {
    case X11DeviceKind::keyboard: caps.keyboard = true; break;
    case X11DeviceKind::mouse: caps.mouse = true; break;
    case X11DeviceKind::gamepad: caps.gamepad = true; break;
    case X11DeviceKind::other: break;
    }
  }
  return caps;
}

std::vector<std::string> X11InputSource::unavailable() const {
  std::vector<std::string> reasons;
  if (!valid()) {
    reasons.push_back("xi2: input adapter is not attached to a display");
    return reasons;
  }
  const auto caps = capabilities();
  if (!caps.keyboard)
    reasons.push_back(
        "xi2: no keyboard device visible to XInput2 (keyboard bindings "
        "stay inactive)");
  if (!caps.mouse)
    reasons.push_back("xi2: no pointer device visible to XInput2 (mouse "
                      "bindings stay inactive)");
  if (!caps.gamepad)
    reasons.push_back("xi2: no gamepad device visible to XInput2 (controller "
                      "bindings stay inactive)");
  return reasons;
}

X11InputPoll X11InputSource::poll() {
  X11InputPoll result;
  if (display_ == nullptr) return result;
  auto *raw = reinterpret_cast<Display *>(display_);
  X11ErrorTrap trap{raw};
  while (XPending(raw) > 0) {
    XEvent event{};
    XNextEvent(raw, &event);
    if (!trap.ok()) break;
    if (event.type == PropertyNotify) {
      if (static_cast<std::uint64_t>(event.xproperty.window) == root_)
        result.windows_changed = true;
      continue;
    }
    if (event.type != GenericEvent || event.xgeneric.extension != xi_opcode_)
      continue;
    if (!XGetEventData(raw, &event.xcookie)) continue;
    const int evtype = event.xcookie.evtype;
    if (evtype == XI_HierarchyChanged) {
      if (refresh_devices()) result.devices_changed = true;
    } else if (evtype == XI_RawKeyPress || evtype == XI_RawKeyRelease ||
               evtype == XI_RawButtonPress || evtype == XI_RawButtonRelease) {
      const auto &raw_event =
          *reinterpret_cast<const XIRawEvent *>(event.xcookie.data);
      const bool pressed =
          evtype == XI_RawKeyPress || evtype == XI_RawButtonPress;
      const auto detail = static_cast<std::uint32_t>(raw_event.detail);
      if (evtype == XI_RawKeyPress || evtype == XI_RawKeyRelease) {
        // Group 0, level 0: the physical key, never the shifted character.
        const KeySym keysym = XkbKeycodeToKeysym(
            raw, static_cast<KeyCode>(detail), 0, 0);
        if (const auto vk = keysym_to_vk(static_cast<std::uint32_t>(keysym));
            vk.has_value())
          result.events.push_back({{layerc::core::InputDevice::keyboard, *vk, 0},
                                   pressed});
      } else {
        const auto kind = kind_of(raw_event.deviceid);
        if (kind == X11DeviceKind::gamepad) {
          std::uint8_t slot{};
          for (std::size_t index = 0; index < gamepads_.size(); ++index)
            if (gamepads_[index] == raw_event.deviceid) {
              slot = static_cast<std::uint8_t>(std::min<std::size_t>(index, 255U));
              break;
            }
          result.events.push_back(
              {{layerc::core::InputDevice::controller,
                static_cast<std::uint16_t>(detail), slot},
               pressed});
        } else {
          result.events.push_back(
              {{layerc::core::InputDevice::mouse, button_to_mouse_code(detail),
                0},
               pressed});
        }
      }
    }
    XFreeEventData(raw, &event.xcookie);
  }
  return result;
}

} // namespace layerc::platform
