#pragma once

#include "x11_display.hpp"

#include "layerc/core/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace layerc::platform {

class X11WindowTracker;

enum class X11DeviceKind : std::uint8_t {
  other,
  keyboard,
  mouse,
  gamepad,
};

struct X11InputDevice {
  int id{};
  std::string name;
  X11DeviceKind kind{X11DeviceKind::other};

  bool operator==(const X11InputDevice &) const = default;
};

// XInput2 publishes device classes, never a device type, so the kind is a
// documented heuristic: keys mean keyboard, a scroll class or a pointer-ish
// name means mouse, and a button device with at least two axes and none of
// that is a gamepad.
// ponytail: name heuristic; switch to udev/libinput gamepad detection when a
// device misclassifies (a joystick exposing scroll axes, for instance).
[[nodiscard]] X11DeviceKind classify_device(std::string_view name, bool has_keys,
                                            bool has_buttons, int valuators,
                                            bool has_scroll) noexcept;

// XKB physical key name to the Windows-authored scan-code token contract:
// AD03 -> 0x12, RCTL -> 0xE01D. Unknown names are not guessed from keycodes
// or layout symbols; the imported profile remains unchanged.
[[nodiscard]] std::optional<std::uint16_t>
xkb_key_name_to_scan_code(std::string_view name) noexcept;

// Conventional X pointer numbering to profile codes: 2/3 -> middle/right
// (3/2), 4/5 -> wheel (6/7), 8/9 -> side (4/5). Zero means unsupported;
// in particular horizontal wheel buttons 6/7 are never side-button tokens.
[[nodiscard]] std::uint16_t button_to_mouse_code(std::uint32_t button) noexcept;

struct X11InputCapabilities {
  bool keyboard{};
  bool mouse{};
  bool gamepad{};
};

struct X11InputPoll {
  std::vector<layerc::core::InputEvent> events;
  // A device appeared, vanished, or changed (XI_HierarchyChanged).
  bool devices_changed{};
  // A root-window property changed: the active window or the client list did.
  bool windows_changed{};
};

// Passive XI2 raw input on the root window. Reads events only — nothing is
// ever synthesized back into the server.
class X11InputSource {
public:
  X11InputSource() = default;
  X11InputSource(X11InputSource &&other) noexcept;
  X11InputSource &operator=(X11InputSource &&other) noexcept;
  X11InputSource(const X11InputSource &) = delete;
  X11InputSource &operator=(const X11InputSource &) = delete;
  ~X11InputSource();

  // Selects raw key/button events plus hierarchy notices. Requires an XI2
  // connection; an empty error means success.
  [[nodiscard]] static std::optional<X11InputSource>
  create(const X11Display &display, std::string &error);

  [[nodiscard]] bool valid() const noexcept { return display_ != nullptr; }
  [[nodiscard]] const std::vector<X11InputDevice> &devices() const noexcept {
    return devices_;
  }
  [[nodiscard]] X11InputCapabilities capabilities() const noexcept;
  // Reasons for missing device classes, limited to the classes `needed`
  // names so an unused class never degrades the input capability.
  [[nodiscard]] std::vector<std::string>
  unavailable(X11InputCapabilities needed = {.keyboard = true,
                                             .mouse = true,
                                             .gamepad = true}) const;

  // Drains everything queued on the connection, converting XI2 raw events
  // into core input events. Unknown events are discarded so a readable
  // socket cannot busy-poll the loop.
  [[nodiscard]] X11InputPoll poll(X11WindowTracker *tracker = nullptr);

  // Rescans the XI2 device roster; true when it changed.
  [[nodiscard]] bool refresh_devices() noexcept;
  // Read the current XKB physical names/aliases; called on map/name notices.
  [[nodiscard]] bool refresh_keymap() noexcept;

private:
  X11InputSource(struct _XDisplay *display, int xi_opcode,
                 std::uint64_t root) noexcept;

  void select_events() noexcept;
  void deselect_events() noexcept;
  [[nodiscard]] X11DeviceKind kind_of(int deviceid) const noexcept;

  struct _XDisplay *display_{};
  int xi_opcode_{};
  std::uint64_t root_{};
  std::vector<X11InputDevice> devices_;
  std::vector<int> gamepads_;
  std::array<std::uint16_t, 256> key_codes_{};
  bool keymap_available_{};
  int xkb_event_base_{};
};

} // namespace layerc::platform
