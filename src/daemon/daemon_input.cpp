#include "daemon_app.hpp"

#include "layerc/core/hid.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace layerc::daemon {
namespace {

std::string_view input_device_label(const layerc::core::InputDevice device) {
  using layerc::core::InputDevice;
  switch (device) {
  case InputDevice::keyboard: return "keyboard";
  case InputDevice::mouse: return "mouse";
  case InputDevice::controller: return "controller";
  case InputDevice::generic_hid: return "hid";
  }
  return "unknown";
}

} // namespace

bool DaemonApp::register_input_devices() {
  const RAWINPUTDEVICE devices[] = {
      {.usUsagePage = 0x01,
       .usUsage = 0x06,
       .dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
       .hwndTarget = window_},
      {.usUsagePage = 0x01,
       .usUsage = 0x02,
       .dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
       .hwndTarget = window_},
  };
  return RegisterRawInputDevices(devices, static_cast<UINT>(std::size(devices)),
                                 sizeof(RAWINPUTDEVICE)) != FALSE;
}

void DaemonApp::unregister_input_devices() noexcept {
  if (!raw_input_registered_) return;
  const RAWINPUTDEVICE devices[] = {
      {.usUsagePage = 0x01,
       .usUsage = 0x06,
       .dwFlags = RIDEV_REMOVE,
       .hwndTarget = nullptr},
      {.usUsagePage = 0x01,
       .usUsage = 0x02,
       .dwFlags = RIDEV_REMOVE,
       .hwndTarget = nullptr},
  };
  (void)RegisterRawInputDevices(devices, static_cast<UINT>(std::size(devices)),
                                sizeof(RAWINPUTDEVICE));
  raw_input_registered_ = false;
}

void DaemonApp::reconcile_input_runtime() {
  const bool needs_system_hid = std::ranges::any_of(
      active_input_sessions_, [](const Session *session) {
        return session != nullptr &&
               std::ranges::any_of(
                   session->profile.hid_bindings, [](const auto &binding) {
                     return binding.allow_system_class &&
                            layerc::core::is_system_keyboard_or_mouse(
                                binding.device);
                   });
      });
  const bool needs_raw_input = std::ranges::any_of(
      active_input_sessions_, [](const Session *session) {
        return session != nullptr && session->uses_raw_input;
      }) || needs_system_hid;
  if (needs_raw_input && !raw_input_registered_)
    raw_input_registered_ = register_input_devices();
  else if (!needs_raw_input)
    unregister_input_devices();

  const bool needs_polling = std::ranges::any_of(
      active_input_sessions_, [](const Session *session) {
        return session != nullptr && session->requires_input_polling();
      });
  if (needs_polling && !input_timer_running_) {
    SetTimer(window_, input_poll_timer, 16, nullptr);
    input_timer_running_ = true;
  } else if (!needs_polling && input_timer_running_) {
    KillTimer(window_, input_poll_timer);
    input_timer_running_ = false;
    controller_states_ = {};
    for (auto &session : sessions_) session->clear_input_state();
  }
}

void DaemonApp::quiesce_input_runtime() noexcept {
  if (input_timer_running_ && window_ != nullptr)
    KillTimer(window_, input_poll_timer);
  input_timer_running_ = false;
  unregister_input_devices();
  controller_states_ = {};
  input_foreground_ = nullptr;
  for (auto &session : sessions_) session->clear_input_state();
}

void DaemonApp::handle_raw_input(const HRAWINPUT handle) {
  UINT size{};
  if (GetRawInputData(handle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 ||
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
  if (GetRawInputData(handle, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) !=
      size) {
    return;
  }

  const auto &input = *reinterpret_cast<const RAWINPUT *>(buffer);
  if (hid_runtime_) handle_hid_input(input);
  if (input.header.dwType == RIM_TYPEKEYBOARD) {
    const auto &keyboard = input.data.keyboard;
    std::uint16_t code = keyboard.MakeCode;
    if ((keyboard.Flags & RI_KEY_E0) != 0) {
      code = static_cast<std::uint16_t>(code | 0xE000U);
    }
    emit_input({.token = {.device = layerc::core::InputDevice::keyboard, .code = code},
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

void DaemonApp::poll_input_devices() {
  if (active_input_sessions_.empty()) return;
  if (GetAncestor(GetForegroundWindow(), GA_ROOT) != input_foreground_) {
    sync_foreground_state();
    return;
  }
  bool changed{};
  for (auto *session : active_input_sessions_) {
    if (session->is_global()) {
      changed = poll_global_keyboard(*session) || changed;
    }
  }
  if (std::any_of(active_input_sessions_.begin(), active_input_sessions_.end(),
                  [](const Session *session) {
                    return session->uses_controller;
                  })) {
    poll_controllers();
  }
  if (changed) update_overlays();
}

bool DaemonApp::poll_global_keyboard(Session &session) {
  bool changed{};
  for (auto &key : session.global_keyboard) {
    const UINT scan = static_cast<UINT>(key.token.code);
    const UINT virtual_key = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
    if (virtual_key == 0) continue;
    const bool pressed =
        (GetAsyncKeyState(static_cast<int>(virtual_key)) & 0x8000) != 0;
    const bool was_pressed = key.pressed;
    key.pressed = pressed;
    if (pressed != was_pressed) {
      changed =
          process_input(session, {.token = key.token, .pressed = pressed}) ||
          changed;
    }
  }
  return changed;
}

void DaemonApp::seed_controller_states() {
  for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
    XINPUT_STATE state{};
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
      state = {};
    }
    controller_states_[index] = state;
  }
}
void DaemonApp::poll_controllers() {
  for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
    XINPUT_STATE state{};
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
      state = {};
    }
    const WORD previous = controller_states_[index].Gamepad.wButtons;
    const WORD current = state.Gamepad.wButtons;
    for (unsigned bit = 0; bit < 16; ++bit) {
      const WORD mask = static_cast<WORD>(1U << bit);
      if ((previous & mask) != (current & mask)) {
        emit_input({.token = {.device = layerc::core::InputDevice::controller,
                              .code = static_cast<std::uint16_t>(bit + 1U),
                              .controller_index = static_cast<std::uint8_t>(index)},
                    .pressed = (current & mask) != 0});
      }
    }
    const auto emit_trigger = [this, index](const BYTE old_value,
                                            const BYTE new_value,
                                            const std::uint16_t code) {
      const bool old_pressed = old_value >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
      const bool new_pressed = new_value >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
      if (old_pressed != new_pressed) {
        emit_input({.token = {.device = layerc::core::InputDevice::controller,
                              .code = code,
                              .controller_index = static_cast<std::uint8_t>(index)},
                    .pressed = new_pressed});
      }
    };
    emit_trigger(controller_states_[index].Gamepad.bLeftTrigger,
                 state.Gamepad.bLeftTrigger, 17);
    emit_trigger(controller_states_[index].Gamepad.bRightTrigger,
                 state.Gamepad.bRightTrigger, 18);
    controller_states_[index] = state;
  }
}

void DaemonApp::emit_input(const layerc::core::InputEvent &event) {
  if (GetAncestor(GetForegroundWindow(), GA_ROOT) != input_foreground_)
    sync_foreground_state();
  if (active_input_sessions_.empty()) return;
  bool changed{};
  for (auto *session : active_input_sessions_)
    changed = process_input(*session, event) || changed;
  if (changed) update_overlays();
}

bool DaemonApp::process_input(Session &session,
                              const layerc::core::InputEvent &event) {
  const auto triggered = session.input.process(event);
  if (!triggered.empty()) {
    session.last_input_match =
        std::string(input_device_label(event.token.device)) +
        ":" + std::to_string(event.token.code) +
        (event.pressed ? ":press" : ":release");
    session.last_input_at = layerc::core::MonotonicClock::now();
  }
  if (session.blocks_all_triggers()) return false;
  const auto now = layerc::core::MonotonicClock::now();
  bool changed{};
  for (const auto &cooldown_id : triggered) {
    std::vector<layerc::core::VisualTriggerRequest> requests;
    if (session.visual_triggers)
      requests = session.visual_triggers->on_input(cooldown_id, now);
    else
      requests.push_back(
          {cooldown_id, layerc::core::VisualTriggerSource::direct_input});
    for (const auto &request : requests) {
      if (session.cooldowns.trigger(request.cooldown_id, now)) {
        if (session.visual_triggers)
          session.visual_triggers->on_cooldown_triggered(
              request.cooldown_id);
        changed = true;
        update_tooltip(
            L"LayerD - triggered " +
            layerc::platform::utf8_to_wide(request.cooldown_id));
      }
    }
  }
  return changed;
}

} // namespace layerc::daemon
