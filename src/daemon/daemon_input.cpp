#include "daemon_state.hpp"

#include "layerc/core/hid.hpp"
#include "layerc/platform/text.hpp"
#include "layerc/platform/xinput_controls.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace layerc::daemon {

// Keyboard and mouse Raw Input register separately: a mouse sink receives
// WM_INPUT for every movement, so it is registered only while something can
// consume mouse input.
void InputRuntime::set_raw_input(const bool keyboard, const bool mouse) noexcept {
  const auto apply = [this](const USHORT usage, const bool wanted,
                            bool &registered) {
    if (wanted == registered)
      return;
    const RAWINPUTDEVICE device{
        .usUsagePage = 0x01,
        .usUsage = usage,
        .dwFlags = static_cast<DWORD>(
            wanted ? RIDEV_INPUTSINK | RIDEV_DEVNOTIFY : RIDEV_REMOVE),
        .hwndTarget = wanted ? window_ : nullptr};
    // Removal is best effort; a failed registration retries on the next
    // reconcile.
    if (RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE ||
        !wanted)
      registered = wanted;
  };
  apply(0x06, keyboard, raw_keyboard_registered);
  apply(0x02, mouse, raw_mouse_registered);
}

layerc::runtime::ServiceReconcileResult
InputRuntime::reconcile(const layerc::runtime::CapabilityDemand &) {
  const bool needs_system_hid =
      std::ranges::any_of(sessions_.active_inputs, [](const Session *session) {
        return session != nullptr &&
               std::ranges::any_of(
                   session->automation.profile().hid_bindings, [](const auto &binding) {
                     return binding.allow_system_class &&
                            layerc::core::is_system_keyboard_or_mouse(
                                binding.device);
                   });
      });
  // Keyboard and mouse registration are independent demands. System-class HID
  // bindings sample both Raw Input streams directly.
  const bool needs_raw_keyboard =
      needs_system_hid ||
      std::ranges::any_of(sessions_.active_inputs,
                          [](const Session *session) {
                            return session != nullptr &&
                                   session->uses_raw_keyboard;
                          });
  const bool needs_raw_mouse =
      needs_system_hid ||
      std::ranges::any_of(sessions_.active_inputs,
                          [](const Session *session) {
                            return session != nullptr &&
                                   session->uses_raw_mouse;
                          });
  set_raw_input(needs_raw_keyboard, needs_raw_mouse);

  const bool needs_polling =
      std::ranges::any_of(sessions_.active_inputs, [](const Session *session) {
        return session != nullptr && session->requires_input_polling();
      });
  if (needs_polling && !timer_running) {
    SetTimer(window_, input_poll_timer, 16, nullptr);
    timer_running = true;
  } else if (!needs_polling && timer_running) {
    KillTimer(window_, input_poll_timer);
    timer_running = false;
    reset_controllers();
    for (auto &session : sessions_.sessions)
      session->clear_input_state();
  }
  return {};
}

void InputRuntime::quiesce() noexcept {
  if (timer_running && window_ != nullptr)
    KillTimer(window_, input_poll_timer);
  timer_running = false;
  set_raw_input(false, false);
  reset_controllers();
  sessions_.input_foreground = nullptr;
  for (auto &session : sessions_.sessions)
    session->clear_input_state();
}

layerc::runtime::RuntimeResourceStats
InputRuntime::resources() const noexcept {
  return {.timers = timer_running ? 1U : 0U};
}

void InputRuntime::handle_raw_input(const HRAWINPUT handle) {
  UINT size{};
  if (GetRawInputData(handle, RID_INPUT, nullptr, &size,
                      sizeof(RAWINPUTHEADER)) != 0 ||
      size == 0) {
    return;
  }
  std::array<std::byte, sizeof(RAWINPUT)> stack_buffer{};
  std::vector<std::byte> heap_buffer;
  std::byte *buffer = stack_buffer.data();
  if (size > stack_buffer.size()) {
    heap_buffer.resize(size);
    buffer = heap_buffer.data();
  }
  if (GetRawInputData(handle, RID_INPUT, buffer, &size,
                      sizeof(RAWINPUTHEADER)) != size) {
    return;
  }

  const auto &input = *reinterpret_cast<const RAWINPUT *>(buffer);
  hid_.handle_input(input);
  if (input.header.dwType == RIM_TYPEKEYBOARD) {
    const auto &keyboard = input.data.keyboard;
    std::uint16_t code = keyboard.MakeCode;
    if ((keyboard.Flags & RI_KEY_E0) != 0) {
      code = static_cast<std::uint16_t>(code | 0xE000U);
    }
    emit_input(
        {.token = {.device = layerc::core::InputDevice::keyboard, .code = code},
         .pressed = (keyboard.Flags & RI_KEY_BREAK) == 0});
  } else if (input.header.dwType == RIM_TYPEMOUSE) {
    const auto flags = input.data.mouse.usButtonFlags;
    const auto emit_button = [this, flags](const USHORT down, const USHORT up,
                                           const std::uint16_t code) {
      if ((flags & down) != 0) {
        emit_input({{layerc::core::InputDevice::mouse, code, 0}, true});
      }
      if ((flags & up) != 0) {
        emit_input({{layerc::core::InputDevice::mouse, code, 0}, false});
      }
    };
    emit_button(RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 1);
    emit_button(RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 2);
    emit_button(RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 3);
    emit_button(RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 4);
    emit_button(RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 5);
    if ((flags & RI_MOUSE_WHEEL) != 0) {
      const auto delta = static_cast<SHORT>(input.data.mouse.usButtonData);
      const std::uint16_t code = delta > 0 ? 6 : 7;
      emit_input({{layerc::core::InputDevice::mouse, code, 0}, true});
      emit_input({{layerc::core::InputDevice::mouse, code, 0}, false});
    }
  }
}

void InputRuntime::poll_input_devices() {
  if (sessions_.active_inputs.empty())
    return;
  if (GetAncestor(GetForegroundWindow(), GA_ROOT) != sessions_.input_foreground) {
    notifications_.sync_foreground_state();
    return;
  }
  for (auto *session : sessions_.active_inputs) {
    if (session->is_global()) {
      poll_global_keyboard(*session);
    }
  }
  if (std::ranges::any_of(sessions_.active_inputs,
                         [](const Session *session) {
                           return session->uses_controller;
                         })) {
    poll_controllers();
  }
}

void InputRuntime::poll_global_keyboard(Session &session) {
  for (auto &key : session.global_keyboard) {
    const bool pressed =
        (GetAsyncKeyState(static_cast<int>(key.virtual_key)) & 0x8000) != 0;
    const bool was_pressed = key.pressed;
    key.pressed = pressed;
    if (pressed != was_pressed) {
      process_input(session, {.token = key.token, .pressed = pressed});
    }
  }
}

void InputRuntime::seed_controller_states() {
  for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
    XINPUT_STATE state{};
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
      state = {};
    }
    controllers_[index] = state;
  }
}
void InputRuntime::poll_controllers() {
  for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
    XINPUT_STATE state{};
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
      state = {};
    }
    auto &previous_state = controllers_[index];
    const auto previous = platform::xinput_digital_controls(previous_state.Gamepad);
    const auto current = platform::xinput_digital_controls(state.Gamepad);
    for (unsigned bit = 0; bit < platform::xinput_digital_control_count; ++bit) {
      const auto mask = std::uint32_t{1} << bit;
      if ((previous & mask) != (current & mask)) {
        emit_input({.token = platform::xinput_control_token(bit, index),
                    .pressed = (current & mask) != 0});
      }
    }

    const auto now = layerc::core::MonotonicClock::now();
    const auto stick = [](const SHORT value) {
      return value < 0 ? static_cast<double>(value) / 32768.0
                       : static_cast<double>(value) / 32767.0;
    };
    const auto trigger = [](const BYTE value) {
      return static_cast<double>(value) / 255.0;
    };
    struct AnalogAxis {
      const char *name;
      double value;
    };
    const AnalogAxis axes[] = {
        {"leftX", stick(state.Gamepad.sThumbLX)},
        {"leftY", stick(state.Gamepad.sThumbLY)},
        {"rightX", stick(state.Gamepad.sThumbRX)},
        {"rightY", stick(state.Gamepad.sThumbRY)},
        {"leftTrigger", trigger(state.Gamepad.bLeftTrigger)},
        {"rightTrigger", trigger(state.Gamepad.bRightTrigger)},
    };
    for (auto *session : sessions_.active_inputs) {
      if (session == nullptr || !session->automation.runtime())
        continue;
      for (const auto &axis : axes)
        automation_.process_input_value(
            *session, layerc::core::controller_axis_source_id(index, axis.name),
            axis.value, now);
    }
    previous_state = state;
  }
}

void InputRuntime::emit_input(const layerc::core::InputEvent &event) {
  if (GetAncestor(GetForegroundWindow(), GA_ROOT) != sessions_.input_foreground)
    notifications_.sync_foreground_state();
  if (sessions_.active_inputs.empty())
    return;
  for (auto *session : sessions_.active_inputs)
    process_input(*session, event);
}

void InputRuntime::process_input(Session &session,
                              const layerc::core::InputEvent &event) {
  const auto now = layerc::core::MonotonicClock::now();
  const auto result = session.automation.feed_input(session.input, event, now);
  if (result.changed)
    overlays_.mark_dirty();
  if (result.matched)
    session.last_input_match =
        std::string(layerc::core::input_device_label(event.token.device)) + ":" +
        std::to_string(event.token.code) +
        (event.pressed ? ":press" : ":release");
  if (!result.triggered.empty())
    notifications_.update_tooltip(L"LayerD - triggered " +
                   layerc::platform::utf8_to_wide(result.triggered.back()));
  if (session.automation.runtime())
    automation_.after_step(session, now);
}

} // namespace layerc::daemon
