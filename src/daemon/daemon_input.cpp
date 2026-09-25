#include "daemon_app.hpp"

#include "layerc/core/hid.hpp"
#include "layerc/platform/text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace layerc::daemon {

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
  if (!input_runtime_.raw_input_registered())
    return;
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
  input_runtime_.set_raw_input_registered(false);
}

void DaemonApp::reconcile_input_runtime() {
  const bool needs_system_hid =
      std::ranges::any_of(session_registry_.active_inputs(), [](const Session *session) {
        return session != nullptr &&
               std::ranges::any_of(
                   session->profile.hid_bindings, [](const auto &binding) {
                     return binding.allow_system_class &&
                            layerc::core::is_system_keyboard_or_mouse(
                                binding.device);
                   });
      });
  const bool needs_raw_input =
      std::ranges::any_of(session_registry_.active_inputs(),
                          [](const Session *session) {
                            return session != nullptr &&
                                   session->uses_raw_input;
                          }) ||
      needs_system_hid;
  if (needs_raw_input && !input_runtime_.raw_input_registered())
    input_runtime_.set_raw_input_registered(register_input_devices());
  else if (!needs_raw_input)
    unregister_input_devices();

  const bool needs_polling =
      std::ranges::any_of(session_registry_.active_inputs(), [](const Session *session) {
        return session != nullptr && session->requires_input_polling();
      });
  if (needs_polling && !input_runtime_.timer_running()) {
    SetTimer(window_, input_poll_timer, 16, nullptr);
    input_runtime_.set_timer_running(true);
  } else if (!needs_polling && input_runtime_.timer_running()) {
    KillTimer(window_, input_poll_timer);
    input_runtime_.set_timer_running(false);
    input_runtime_.reset_controllers();
    for (auto &session : session_registry_.sessions())
      session->clear_input_state();
  }
}

void DaemonApp::quiesce_input_runtime() noexcept {
  if (input_runtime_.timer_running() && window_ != nullptr)
    KillTimer(window_, input_poll_timer);
  input_runtime_.set_timer_running(false);
  unregister_input_devices();
  input_runtime_.reset_controllers();
  session_registry_.set_input_foreground(nullptr);
  for (auto &session : session_registry_.sessions())
    session->clear_input_state();
}

void DaemonApp::handle_raw_input(const HRAWINPUT handle) {
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
  if (input_runtime_.hid())
    handle_hid_input(input);
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

void DaemonApp::poll_input_devices() {
  if (session_registry_.active_inputs().empty())
    return;
  if (GetAncestor(GetForegroundWindow(), GA_ROOT) != session_registry_.input_foreground()) {
    sync_foreground_state();
    return;
  }
  for (auto *session : session_registry_.active_inputs()) {
    if (session->is_global()) {
      poll_global_keyboard(*session);
    }
  }
  if (std::any_of(
          session_registry_.active_inputs().begin(), session_registry_.active_inputs().end(),
          [](const Session *session) { return session->uses_controller; })) {
    poll_controllers();
  }
}

void DaemonApp::poll_global_keyboard(Session &session) {
  for (auto &key : session.global_keyboard) {
    const UINT scan = static_cast<UINT>(key.token.code);
    const UINT virtual_key = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
    if (virtual_key == 0)
      continue;
    const bool pressed =
        (GetAsyncKeyState(static_cast<int>(virtual_key)) & 0x8000) != 0;
    const bool was_pressed = key.pressed;
    key.pressed = pressed;
    if (pressed != was_pressed) {
      process_input(session, {.token = key.token, .pressed = pressed});
    }
  }
}

void DaemonApp::seed_controller_states() {
  for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
    XINPUT_STATE state{};
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
      state = {};
    }
    input_runtime_.controllers()[index] = state;
  }
}
void DaemonApp::poll_controllers() {
  for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
    XINPUT_STATE state{};
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
      state = {};
    }
    auto &previous_state = input_runtime_.controllers()[index];
    const WORD previous = previous_state.Gamepad.wButtons;
    const WORD current = state.Gamepad.wButtons;
    for (unsigned bit = 0; bit < 16; ++bit) {
      const WORD mask = static_cast<WORD>(1U << bit);
      if ((previous & mask) != (current & mask)) {
        emit_input(
            {.token = {.device = layerc::core::InputDevice::controller,
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
        emit_input(
            {.token = {.device = layerc::core::InputDevice::controller,
                       .code = code,
                       .controller_index = static_cast<std::uint8_t>(index)},
             .pressed = new_pressed});
      }
    };
    emit_trigger(previous_state.Gamepad.bLeftTrigger,
                 state.Gamepad.bLeftTrigger, 17);
    emit_trigger(previous_state.Gamepad.bRightTrigger,
                 state.Gamepad.bRightTrigger, 18);

    const auto now = layerc::core::MonotonicClock::now();
    const auto stick = [](const SHORT value) {
      return value < 0 ? static_cast<double>(value) / 32768.0
                       : static_cast<double>(value) / 32767.0;
    };
    const auto trigger = [](const BYTE value) {
      return static_cast<double>(value) / 255.0;
    };
    const auto prefix = "controller:" + std::to_string(index) + ":";
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
    for (auto *session : session_registry_.active_inputs()) {
      if (session == nullptr || !session->automation_runtime)
        continue;
      for (const auto &axis : axes)
        process_automation_input_value(*session, prefix + axis.name,
                                       axis.value, now);
    }
    previous_state = state;
  }
}

void DaemonApp::emit_input(const layerc::core::InputEvent &event) {
  if (GetAncestor(GetForegroundWindow(), GA_ROOT) != session_registry_.input_foreground())
    sync_foreground_state();
  if (session_registry_.active_inputs().empty())
    return;
  for (auto *session : session_registry_.active_inputs())
    process_input(*session, event);
}

void DaemonApp::process_automation_input_value(
    Session &session, std::string source, double value,
    const layerc::core::TimePoint now, const bool momentary) {
  if (!session.automation_runtime || !std::isfinite(value))
    return;
  const auto &profile = session.automation_runtime->profile();
  const bool demanded =
      std::ranges::any_of(profile.automations, [&](const auto &automation) {
        return automation.enabled &&
               std::ranges::any_of(automation.rules, [&](const auto &rule) {
                 return rule.event == layerc::core::AutomationEventKind::
                                          input_value_changed &&
                        (rule.source_filter.empty() ||
                         rule.source_filter == source);
               });
      });
  if (!demanded)
    return;

  if (std::abs(value) < profile.continuous_input.deadzone)
    value = 0.0;
  const auto minimum_interval =
      layerc::core::Milliseconds(std::max<std::uint32_t>(
          1U,
          1'000U / std::max<std::uint32_t>(
                       1U, profile.continuous_input.max_events_per_second)));
  auto &edges = session.automation_runtime->edges();
  const auto last = edges.input_emitted.find(source);
  if (last != edges.input_emitted.end() &&
      now - last->second < minimum_interval)
    return;
  const auto emit_value = [&](const double previous_value) {
    edges.input_emitted[source] = now;
    auto event = automation_event(
        layerc::core::AutomationEventKind::input_value_changed,
        std::move(source), now);
    event.fields.emplace("previous",
                         layerc::core::AutomationValue(previous_value));
    event.fields.emplace("value", layerc::core::AutomationValue(value));
    process_automation_event(session, event);
  };
  if (momentary) {
    emit_value(0.0);
    return;
  }
  const auto previous = edges.input_values.find(source);
  if (previous == edges.input_values.end()) {
    edges.input_values.emplace(std::move(source), value);
    return;
  }
  if (std::abs(value - previous->second) <
      profile.continuous_input.change_threshold)
    return;

  const double old_value = previous->second;
  previous->second = value;
  emit_value(old_value);
}
void DaemonApp::process_input(Session &session,
                              const layerc::core::InputEvent &event) {
  const auto now = layerc::core::MonotonicClock::now();
  const auto result = session.feed_input(session.input, event,
                                         session.profile, now);
  if (result.changed)
    mark_overlays_dirty();
  if (result.matched)
    session.last_input_match =
        std::string(layerc::core::input_device_label(event.token.device)) + ":" +
        std::to_string(event.token.code) +
        (event.pressed ? ":press" : ":release");
  if (!result.triggered.empty())
    update_tooltip(L"LayerD - triggered " +
                   layerc::platform::utf8_to_wide(result.triggered.back()));
  if (session.automation_runtime) {
    publish_automation_trace(session);
    synchronize_automation_state(session, now);
    schedule_automation_timer();
  }
}

} // namespace layerc::daemon
