// Passive XI2 raw input (V2.6.1): raw key/button events on the root window,
// normalized into the core input token space, plus hierarchy tracking for
// hotplug. Reads only — no event is ever synthesized back into the server.
#include "x11_input.hpp"

#include "x11_error.hpp"
#include "x11_window.hpp"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace layerc::platform {
namespace {

struct PhysicalKey {
  std::string_view name;
  std::uint16_t scan_code;
};

// XKB key names describe physical positions, independently of layout symbols
// and server keycode numbering. Match the existing Raw Input MakeCode | E0
// contract; never infer a scan code by subtracting a keycode offset.
constexpr PhysicalKey physical_keys[] = {
    {"ESC", 0x01}, {"AE01", 0x02}, {"AE02", 0x03}, {"AE03", 0x04},
    {"AE04", 0x05}, {"AE05", 0x06}, {"AE06", 0x07}, {"AE07", 0x08},
    {"AE08", 0x09}, {"AE09", 0x0A}, {"AE10", 0x0B}, {"AE11", 0x0C},
    {"AE12", 0x0D}, {"BKSP", 0x0E}, {"TAB", 0x0F}, {"AD01", 0x10},
    {"AD02", 0x11}, {"AD03", 0x12}, {"AD04", 0x13}, {"AD05", 0x14},
    {"AD06", 0x15}, {"AD07", 0x16}, {"AD08", 0x17}, {"AD09", 0x18},
    {"AD10", 0x19}, {"AD11", 0x1A}, {"AD12", 0x1B}, {"RTRN", 0x1C},
    {"LCTL", 0x1D}, {"AC01", 0x1E}, {"AC02", 0x1F}, {"AC03", 0x20},
    {"AC04", 0x21}, {"AC05", 0x22}, {"AC06", 0x23}, {"AC07", 0x24},
    {"AC08", 0x25}, {"AC09", 0x26}, {"AC10", 0x27}, {"AC11", 0x28},
    {"TLDE", 0x29}, {"LFSH", 0x2A}, {"BKSL", 0x2B}, {"AB01", 0x2C},
    {"AB02", 0x2D}, {"AB03", 0x2E}, {"AB04", 0x2F}, {"AB05", 0x30},
    {"AB06", 0x31}, {"AB07", 0x32}, {"AB08", 0x33}, {"AB09", 0x34},
    {"AB10", 0x35}, {"RTSH", 0x36}, {"KPMU", 0x37}, {"LALT", 0x38},
    {"SPCE", 0x39}, {"CAPS", 0x3A}, {"FK01", 0x3B}, {"FK02", 0x3C},
    {"FK03", 0x3D}, {"FK04", 0x3E}, {"FK05", 0x3F}, {"FK06", 0x40},
    {"FK07", 0x41}, {"FK08", 0x42}, {"FK09", 0x43}, {"FK10", 0x44},
    {"NMLK", 0x45}, {"PAUS", 0x45}, {"SCLK", 0x46}, {"KP7", 0x47},
    {"KP8", 0x48}, {"KP9", 0x49}, {"KPSU", 0x4A}, {"KP4", 0x4B},
    {"KP5", 0x4C}, {"KP6", 0x4D}, {"KPAD", 0x4E}, {"KP1", 0x4F},
    {"KP2", 0x50}, {"KP3", 0x51}, {"KP0", 0x52}, {"KPDL", 0x53},
    {"LSGT", 0x56}, {"FK11", 0x57}, {"FK12", 0x58}, {"FK13", 0x64},
    {"FK14", 0x65}, {"FK15", 0x66}, {"FK16", 0x67}, {"FK17", 0x68},
    {"FK18", 0x69}, {"FK19", 0x6A}, {"FK20", 0x6B}, {"FK21", 0x6C},
    {"FK22", 0x6D}, {"FK23", 0x6E}, {"FK24", 0x76},
    {"KPEN", 0xE01C}, {"RCTL", 0xE01D}, {"KPDV", 0xE035},
    {"PRSC", 0xE037}, {"RALT", 0xE038}, {"HOME", 0xE047},
    {"UP", 0xE048}, {"PGUP", 0xE049}, {"LEFT", 0xE04B},
    {"RGHT", 0xE04D}, {"END", 0xE04F}, {"DOWN", 0xE050},
    {"PGDN", 0xE051}, {"INS", 0xE052}, {"DELE", 0xE053},
    {"LWIN", 0xE05B}, {"RWIN", 0xE05C}, {"MENU", 0xE05D},
};

constexpr unsigned int keymap_events = XkbNewKeyboardNotifyMask |
    XkbMapNotifyMask | XkbNamesNotifyMask;

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

std::optional<std::uint16_t>
xkb_key_name_to_scan_code(std::string_view name) noexcept {
  while (!name.empty() && (name.back() == '\0' || name.back() == ' '))
    name.remove_suffix(1);
  for (const auto &key : physical_keys)
    if (key.name == name) return key.scan_code;
  return std::nullopt;
}

std::uint16_t button_to_mouse_code(const std::uint32_t button) noexcept {
  switch (button) {
  case 1: return 1;
  case 2: return 3; // X middle -> profile middle
  case 3: return 2; // X right -> profile right
  case 4: return 6; // X wheel up -> profile wheel up
  case 5: return 7; // X wheel down -> profile wheel down
  case 8: return 4; // X back -> profile X1
  case 9: return 5; // X forward -> profile X2
  default: return 0; // Horizontal wheel and unknown buttons have no token.

  }
}

X11InputSource::X11InputSource(struct _XDisplay *display, const int xi_opcode,
                               const std::uint64_t root) noexcept
    : display_(display), xi_opcode_(xi_opcode), root_(root) {}

X11InputSource::X11InputSource(X11InputSource &&other) noexcept
    : display_(std::exchange(other.display_, nullptr)),
      xi_opcode_(std::exchange(other.xi_opcode_, 0)),
      root_(std::exchange(other.root_, 0)), devices_(std::move(other.devices_)),
      gamepads_(std::move(other.gamepads_)),
      key_codes_(other.key_codes_),
      keymap_available_(std::exchange(other.keymap_available_, false)),
      xkb_event_base_(std::exchange(other.xkb_event_base_, 0)) {}

X11InputSource &X11InputSource::operator=(X11InputSource &&other) noexcept {
  if (this != &other) {
    deselect_events();
    display_ = std::exchange(other.display_, nullptr);
    xi_opcode_ = std::exchange(other.xi_opcode_, 0);
    root_ = std::exchange(other.root_, 0);
    devices_ = std::move(other.devices_);
    gamepads_ = std::move(other.gamepads_);
    key_codes_ = other.key_codes_;
    keymap_available_ = std::exchange(other.keymap_available_, false);
    xkb_event_base_ = std::exchange(other.xkb_event_base_, 0);
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
  int xkb_opcode{};
  int xkb_error{};
  int xkb_major = XkbMajorVersion;
  int xkb_minor = XkbMinorVersion;
  if (XkbQueryExtension(display.raw(), &xkb_opcode, &source.xkb_event_base_,
                        &xkb_error, &xkb_major, &xkb_minor)) {
    XkbSelectEvents(display.raw(), XkbUseCoreKbd, keymap_events, keymap_events);
    (void)source.refresh_keymap();
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
  // Select all devices for hotplug and unattached gamepads. Event decoding
  // uses the slave-device roster to ignore master-forwarded duplicates.
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
  if (xkb_event_base_ != 0)
    XkbSelectEvents(raw, XkbUseCoreKbd, keymap_events, 0);
  (void)trap.ok();
  display_ = nullptr;
}

X11DeviceKind X11InputSource::kind_of(const int deviceid) const noexcept {
  for (const auto &device : devices_)
    if (device.id == deviceid) return device.kind;
  return X11DeviceKind::other;
}

std::optional<std::vector<X11InputDevice>>
query_input_devices(struct _XDisplay *display) {
  if (display == nullptr) return std::nullopt;
  auto *raw = reinterpret_cast<Display *>(display);
  int count{};
  X11ErrorTrap trap{raw};
  XIDeviceInfo *info = XIQueryDevice(raw, XIAllDevices, &count);
  if (info == nullptr || !trap.ok()) {
    if (info != nullptr) XIFreeDeviceInfo(info);
    return std::nullopt;
  }
  std::vector<X11InputDevice> next;
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
  return next;
}

std::vector<int> gamepad_ids(const std::vector<X11InputDevice> &devices) {
  std::vector<int> gamepads;
  for (const auto &device : devices)
    if (device.kind == X11DeviceKind::gamepad) gamepads.push_back(device.id);
  return gamepads;
}

layerc::core::InputToken gamepad_button_token(const std::vector<int> &gamepads,
                                              const int deviceid,
                                              const std::uint32_t detail) noexcept {
  std::uint8_t slot{};
  for (std::size_t index = 0; index < gamepads.size(); ++index)
    if (gamepads[index] == deviceid) {
      slot = static_cast<std::uint8_t>(std::min<std::size_t>(index, 255U));
      break;
    }
  return {layerc::core::InputDevice::controller, static_cast<std::uint16_t>(detail), slot};
}

bool X11InputSource::refresh_devices() noexcept {
  // The queries allocate; on failure the previous device lists stay current.
  try {
    auto next = query_input_devices(display_);
    if (!next) return false;
    auto gamepads = gamepad_ids(*next);
    const bool changed = *next != devices_ || gamepads != gamepads_;
    devices_ = std::move(*next);
    gamepads_ = std::move(gamepads);
    return changed;
  } catch (...) {
    return false;
  }
}

bool X11InputSource::refresh_keymap() noexcept {
  key_codes_.fill(0);
  keymap_available_ = false;
  if (display_ == nullptr || xkb_event_base_ == 0) return false;
  auto *raw = reinterpret_cast<Display *>(display_);
  X11ErrorTrap trap{raw};
  auto *keyboard = XkbGetMap(raw, 0, XkbUseCoreKbd);
  if (keyboard == nullptr) return false;
  if (XkbGetNames(raw, XkbKeyNamesMask | XkbKeyAliasesMask, keyboard) == Success &&
      keyboard->names != nullptr && keyboard->names->keys != nullptr && trap.ok()) {
    for (unsigned int code = keyboard->min_key_code;
         code <= keyboard->max_key_code && code < key_codes_.size(); ++code) {
      const auto name = std::string_view{keyboard->names->keys[code].name, 4};
      auto scan = xkb_key_name_to_scan_code(name);
      for (unsigned int index = 0; !scan && index < keyboard->names->num_key_aliases;
           ++index) {
        const auto &alias = keyboard->names->key_aliases[index];
        if (std::string_view{alias.real, 4} == name)
          scan = xkb_key_name_to_scan_code(std::string_view{alias.alias, 4});
      }
      if (scan) key_codes_[code] = *scan;
    }
    keymap_available_ = true;
  }
  XkbFreeKeyboard(keyboard, XkbAllComponentsMask, True);
  return keymap_available_;
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

std::vector<std::string>
X11InputSource::unavailable(const X11InputCapabilities needed) const {
  std::vector<std::string> reasons;
  if (!valid()) {
    reasons.push_back("xi2: input adapter is not attached to a display");
    return reasons;
  }
  const auto caps = capabilities();
  if (needed.keyboard && !caps.keyboard)
    reasons.push_back(
        "xi2: no keyboard device visible to XInput2 (keyboard bindings "
        "stay inactive)");
  if (needed.keyboard && caps.keyboard && !keymap_available_)
    reasons.push_back("xi2: XKB physical key names unavailable (keyboard bindings stay inactive)");
  if (needed.mouse && !caps.mouse)
    reasons.push_back("xi2: no pointer device visible to XInput2 (mouse "
                      "bindings stay inactive)");
  if (needed.gamepad && !caps.gamepad)
    reasons.push_back("xi2: no gamepad device visible to XInput2 (controller "
                      "bindings stay inactive)");
  return reasons;
}

X11InputPoll X11InputSource::poll(X11WindowTracker *tracker) {
  X11InputPoll result;
  if (display_ == nullptr) return result;
  auto *raw = reinterpret_cast<Display *>(display_);
  X11Display::dispatch_events(raw, [&](XEvent &event) {
    // Discovery sees every event even while input owns the active decoder.
    if (tracker != nullptr)
      result.windows_changed = tracker->observe(event) || result.windows_changed;
    if (xkb_event_base_ != 0 && event.type == xkb_event_base_) {
      (void)refresh_keymap();
      return;
    }
    if (event.type == PropertyNotify) {
      if (static_cast<std::uint64_t>(event.xproperty.window) == root_)
        result.windows_changed = true;
      return;
    }
    if (event.type != GenericEvent || event.xgeneric.extension != xi_opcode_)
      return;
    if (!XGetEventData(raw, &event.xcookie)) return;
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
        if (kind_of(raw_event.deviceid) == X11DeviceKind::keyboard &&
            detail < key_codes_.size() && key_codes_[detail] != 0)
          result.events.push_back(
              {{layerc::core::InputDevice::keyboard, key_codes_[detail], 0}, pressed});
      } else {
        const auto kind = kind_of(raw_event.deviceid);
        if (kind == X11DeviceKind::gamepad) {
          result.events.push_back(
              {gamepad_button_token(gamepads_, raw_event.deviceid, detail), pressed});
        } else if (kind == X11DeviceKind::mouse) {
          const auto code = button_to_mouse_code(detail);
          if (code != 0)
            result.events.push_back(
                {{layerc::core::InputDevice::mouse, code, 0}, pressed});
        }
      }
    }
    XFreeEventData(raw, &event.xcookie);
  });
  return result;
}

} // namespace layerc::platform
